module clice;

import :feature.document_links;
import :feature.hierarchy;
import :feature.navigation;
import :feature.protocol;
import :feature.workspace_symbols;
import :server.client;
import :server.extension;
import :server.master;
import :server.served_project;
import :support.json;

namespace clice {

using Cursor = index::IndexQuery::Cursor;

async::task<codec::raw_value, ipc::Error> stop_reply(GateStop stop) {
    if(stop.error) {
        co_await async::fail(std::move(*stop.error));
    }
    co_return codec::raw_value{"null"};
}

/// The indexed symbol a hierarchy item names, or the error saying it names
/// none.
template <typename Item>
static std::expected<index::SymbolHash, ipc::Error>
    item_symbol(const ServedProject& project, Fid path_id, const Item& item, llvm::StringRef kind) {
    auto symbol = feature::item_symbol(project.query(),
                                       path_id,
                                       project.cursor_at(path_id, item.selection_range.start),
                                       item.data);
    if(!symbol) {
        return std::unexpected(ipc::Error{protocol::ErrorCode::InvalidParams,
                                          std::format("Failed to resolve {} item", kind)});
    }
    return *symbol;
}

void LSPClient::register_navigation() {
    // The index answers navigation for a document that is not open, whose
    // ticket has no session: an empty result is a real answer, never an
    // error.

    // Go-to-definition, assembled across all providers: preamble directive
    // targets, the index, and the directives the worker's AST sees, with an
    // index retry after the compile refreshes a dirty document.
    on_document<protocol::DefinitionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, served, version, ticket] = document;
            auto& session = ticket.session;
            auto& project = *served;
            auto& position = params.position;
            auto encoding = server.position_encoding;
            auto& files = project.project.file_table;
            if(session) {
                if(auto stop = co_await project.navigation_gate(ticket)) {
                    co_return co_await stop_reply(std::move(*stop));
                }
            }

            // Preamble include lines first: they have no symbol occurrence
            // in the index and are invisible to the worker's AST. A latest
            // compile still not current after the awaited one means the
            // world was re-dirtied mid-flight (the attempt committed as
            // bounded staleness): the cached links may describe a pre-edit
            // preamble — skip, and let the index and worker paths below
            // answer.
            auto offset = session ? session->positions().offset(position, encoding) : std::nullopt;
            if(offset && project.current(path_id)) {
                auto links = project.preamble_links(*session);
                if(auto* link = feature::link_at(links, *offset)) {
                    co_return to_raw(std::vector{feature::directive_location(files, *link)});
                }
            }

            // The eager index query is safe for dirty documents too:
            // freshness clause 4 serves their shard only while the buffer is
            // byte-identical, and rejects the mixed-view lookup otherwise.
            auto index_definition = [&]() -> std::vector<protocol::Location> {
                auto cursor = project.cursor_at(path_id, position);
                if(!cursor) {
                    return {};
                }
                return feature::to_locations(feature::definition(project.query_all(), *cursor),
                                             encoding);
            };
            if(auto result = index_definition(); !result.empty()) {
                co_return to_raw(result);
            }

            // A closed file has no worker to ask: the index's answer is the
            // answer.
            if(!session) {
                co_return codec::raw_value{"[]"};
            }

            // A document that is not compiled never owes the compile the
            // worker query implies, a document served under freshness clause
            // 4 (compiled, compile still in flight) already went to the
            // index, and a document whose compile a crash stops cannot reach
            // a worker at all. What the worker's answer covers — include
            // directives, which have no symbol occurrence — the manifest
            // edges answer instead, under the same content gate as the
            // index's links: manifest lines are meaningless against a buffer
            // the index never described.
            auto query = project.query();
            auto serving = query.serving(path_id);
            bool shard_served = serving && serving->kind == index::IndexSource::Kind::Shard;
            if(!project.compiles(*session) || project.compile_stopped(*session) ||
               (shard_served && !project.cursor_at(path_id, position))) {
                if(offset) {
                    auto links = feature::document_links(project.text_of(*session), query, path_id);
                    if(auto* link = feature::link_at(links, *offset)) {
                        co_return to_raw(std::vector{feature::directive_location(files, *link)});
                    }
                }
                co_return codec::raw_value{"[]"};
            }

            // A symbol the shard names and no index places waits for the
            // compile: a module's declaration before the module's unit is
            // indexed is in the compile's own index alone. Awaited here, as
            // the links query skips the compile while a crash stops link
            // queries.
            if(shard_served) {
                if(auto stop = co_await project.await_compile(ticket)) {
                    co_return co_await stop_reply(std::move(*stop));
                }
            }

            // A worker error is final: a ContentModified in particular must
            // not be replaced by an index answer computed on the newer
            // buffer.
            auto links = co_await project.directive_links(ticket, ctx.cancellation).or_fail();
            if(auto* link = offset ? feature::link_at(links, *offset) : nullptr) {
                co_return to_raw(std::vector{feature::directive_location(files, *link)});
            }

            // The buffer compiled since the first lookup: retry against the
            // refreshed compile, but only when the compile actually
            // completed — a failed compile leaves the latest one non-current
            // and the caches stale.
            if(project.current(path_id)) {
                if(auto retry = index_definition(); !retry.empty()) {
                    co_return to_raw(retry);
                }
            }
            co_return codec::raw_value{"[]"};
        },
        {.closed = true});

    on_document<protocol::ReferenceParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_await project.compile_outdated();
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> NavigationReply {
                    auto limit = at_most(project.project.settings.references.limit);
                    return to_raw(feature::to_locations(
                        feature::references(project.query_all(),
                                            cursor,
                                            params.context.include_declaration,
                                            limit),
                        server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::DocumentHighlightParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> NavigationReply {
                    return to_raw(
                        feature::to_protocol(feature::document_highlights(project.query(), cursor),
                                             server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::DeclarationParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(project,
                                        document.ticket,
                                        document.path_id,
                                        params.position,
                                        "[]",
                                        [&](const Cursor& cursor) -> NavigationReply {
                                            return to_raw(feature::to_locations(
                                                feature::declaration(project.query_all(), cursor),
                                                server.position_encoding));
                                        });
        },
        {.closed = true});

    on_document<protocol::TypeDefinitionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> NavigationReply {
                    return to_raw(
                        feature::to_locations(feature::type_definition(project.query_all(), cursor),
                                              server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::ImplementationParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_await project.compile_outdated();
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> NavigationReply {
                    return to_raw(
                        feature::to_locations(feature::implementation(project.query_all(), cursor),
                                              server.position_encoding));
                });
        },
        {.closed = true});

    for(auto [method, callee]: {
            std::pair{"$clice/caller", false},
            std::pair{"$clice/callee", true }
    }) {
        on_request<ext::CallTreeParams>(
            method,
            [this, callee](RequestContext& ctx, const ext::CallTreeParams& params) -> RawResult {
                server.pool.user_activity();
                auto empty = [&] {
                    return codec::raw_value{params.hierarchy ? "null" : "[]"};
                };
                if(params.call_type < 0 || params.call_type > 3 || params.levels < 0) {
                    co_await async::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                    "Invalid callType or levels"});
                }
                feature::CallTreeOptions options{
                    .callee = callee,
                    .call_type = params.call_type,
                    .qualified = params.qualified,
                    .levels = params.levels,
                };
                auto reply = [&](const index::IndexQuery& query,
                                 index::SymbolHash symbol,
                                 Fid anchor) -> codec::raw_value {
                    auto tree = feature::call_tree(query,
                                                   symbol,
                                                   anchor,
                                                   options,
                                                   server.position_encoding);
                    if(params.hierarchy) {
                        return to_raw(tree);
                    }
                    std::vector<protocol::Location> locations;
                    if(tree) {
                        std::vector<const feature::CallTreeNode*> pending;
                        for(auto& child: tree->children) {
                            pending.push_back(&child);
                        }
                        while(!pending.empty()) {
                            auto* node = pending.back();
                            pending.pop_back();
                            locations.push_back(node->location);
                            for(auto& child: node->children) {
                                pending.push_back(&child);
                            }
                        }
                    }
                    auto key = [](const protocol::Location& location) {
                        return std::tuple{location.uri,
                                          location.range.start.line,
                                          location.range.start.character,
                                          location.range.end.line,
                                          location.range.end.character};
                    };
                    ranges::sort(locations, {}, key);
                    locations.erase(ranges::unique(locations, {}, key).begin(), locations.end());
                    return to_raw(locations);
                };
                if(!params.id.empty()) {
                    index::SymbolHash symbol = 0;
                    auto [end, error] = std::from_chars(params.id.data(),
                                                        params.id.data() + params.id.size(),
                                                        symbol);
                    if(error != std::errc{} || end != params.id.data() + params.id.size()) {
                        co_await async::fail(
                            ipc::Error{protocol::ErrorCode::InvalidParams, "Invalid call id"});
                    }
                    if(symbol >= std::numeric_limits<index::SymbolHash>::max() - 1) {
                        co_return empty();
                    }
                    // An id identifies the symbol alone. Find its serving project
                    // even when the client supplies no document on expansion.
                    auto projects = server.projects;
                    for(auto& project: projects) {
                        if(project->broken()) {
                            continue;
                        }
                        co_await project->compile_outdated();
                        auto query = project->query_all();
                        if(query.resolve(symbol, Fid{})) {
                            co_return reply(query, symbol, Fid{});
                        }
                    }
                    co_return empty();
                }
                if(!params.text_document || !params.position) {
                    co_await async::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                    "Expected textDocument and position or id"});
                }
                auto document = resolve_uri(params.text_document->uri);
                auto& project = *document.project;
                if(project.broken()) {
                    co_return empty();
                }
                co_await project.compile_outdated();
                co_return co_await navigate(
                    project,
                    document.ticket,
                    document.path_id,
                    *params.position,
                    params.hierarchy ? "null" : "[]",
                    [&](const Cursor& cursor) -> NavigationReply {
                        auto query = project.query_all();
                        for(auto& located: query.resolve_at(cursor)) {
                            if(index::is_function(located.symbol.kind)) {
                                return reply(query, located.symbol.hash, document.path_id);
                            }
                        }
                        return empty();
                    });
            });
    }

    on_document<protocol::CallHierarchyPrepareParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(project,
                                        document.ticket,
                                        document.path_id,
                                        params.position,
                                        "null",
                                        [&](const Cursor& cursor) -> NavigationReply {
                                            auto items = feature::call_hierarchy_items(
                                                project.query_all(),
                                                cursor,
                                                server.position_encoding);
                                            if(items.empty()) {
                                                return codec::raw_value{"null"};
                                            }
                                            return to_raw(items);
                                        });
        },
        {.closed = true});

    on_document<protocol::CallHierarchyIncomingCallsParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_await project.compile_outdated();
            auto symbol = item_symbol(project, document.path_id, params.item, "call hierarchy");
            if(!symbol) {
                co_await async::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::incoming_calls(project.query_all(),
                                                     *symbol,
                                                     document.path_id,
                                                     server.position_encoding));
        },
        {.closed = true});

    on_document<protocol::CallHierarchyOutgoingCallsParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_await project.compile_outdated();
            auto symbol = item_symbol(project, document.path_id, params.item, "call hierarchy");
            if(!symbol) {
                co_await async::fail(std::move(symbol.error()));
            }
            auto name = project.cursor_at(document.path_id, params.item.selection_range.start);
            co_return to_raw(
                feature::outgoing_calls(project.query_all(),
                                        *symbol,
                                        document.path_id,
                                        name ? std::optional(name->site) : std::nullopt,
                                        server.position_encoding));
        },
        {.closed = true});

    on_document<protocol::TypeHierarchyPrepareParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(project,
                                        document.ticket,
                                        document.path_id,
                                        params.position,
                                        "null",
                                        [&](const Cursor& cursor) -> NavigationReply {
                                            auto items = feature::type_hierarchy_items(
                                                project.query_all(),
                                                cursor,
                                                server.position_encoding);
                                            if(items.empty()) {
                                                return codec::raw_value{"null"};
                                            }
                                            return to_raw(items);
                                        });
        },
        {.closed = true});

    on_document<protocol::TypeHierarchySupertypesParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_await project.compile_outdated();
            auto symbol = item_symbol(project, document.path_id, params.item, "type hierarchy");
            if(!symbol) {
                co_await async::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::supertypes(project.query_all(),
                                                 *symbol,
                                                 document.path_id,
                                                 server.position_encoding));
        },
        {.closed = true});

    on_document<protocol::TypeHierarchySubtypesParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_await project.compile_outdated();
            auto symbol = item_symbol(project, document.path_id, params.item, "type hierarchy");
            if(!symbol) {
                co_await async::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::subtypes(project.query_all(),
                                               *symbol,
                                               document.path_id,
                                               server.position_encoding));
        },
        {.closed = true});

    // workspace/symbol over every project: each project's ranked matches,
    // interleaved rank by rank, a symbol two projects index listed once.
    on_request<protocol::WorkspaceSymbolParams>(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            server.pool.user_activity();
            // Another checkout of the same code opened beside it (a
            // worktree) holds a copy of each symbol at the same place in the
            // same text of its folder: the copy of a folder with open
            // documents, the one being edited, stands for them.
            auto ordered = server.projects;
            ranges::stable_partition(ordered, [&](const std::shared_ptr<ServedProject>& project) {
                return ranges::any_of(server.documents, [&](const auto& entry) {
                    return entry.second.owner == project.get();
                });
            });
            llvm::SmallVector<std::vector<feature::WorkspaceSymbol>> ranked;
            for(auto& project: ordered) {
                ranked.push_back(feature::workspace_symbols(project->query_all(),
                                                            params.query,
                                                            Spelling(project->project.root),
                                                            server.position_encoding));
            }
            co_return to_raw(feature::merge_workspace_symbols(ranked));
        });
}

}  // namespace clice
