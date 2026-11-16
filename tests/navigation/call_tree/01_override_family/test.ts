import { expect } from "vitest";
import type { Location } from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

interface Node {
    id: string;
    name: string;
    location: Location;
    callType: number;
    numChildren: number;
    children: Node[];
}

test("override family", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    const root = t.marker("middle");
    const request = (extra: object = {}) =>
        s.request<Node>("$clice/caller", root, { hierarchy: true, ...extra });
    const names = (node: Node) => node.children.map((c) => [c.name, c.callType]);
    const all = await request();
    expect(all.name).toBe("Middle::run");
    expect(names(all)).toEqual([
        ["call_base", 1],
        ["call_middle", 0],
        ["call_derived", 2],
        ["call_qualified", 1],
    ]);
    expect(all.numChildren).toBe(5);
    expect(all.children.every((c) => c.children.length === 0)).toBe(true);
    expect(names(await request({ callType: 0 }))).toEqual([["call_middle", 0]]);
    expect(names(await request({ callType: 1 }))).toEqual([
        ["call_base", 1],
        ["call_middle", 0],
        ["call_qualified", 1],
    ]);
    expect(names(await request({ callType: 2 }))).toEqual([
        ["call_middle", 0],
        ["call_derived", 2],
    ]);
    const fromBase = await s.request<Node>("$clice/caller", t.marker("base"), { hierarchy: true });
    expect(names(fromBase)).toContainEqual(["call_derived", 2]);
    const fromDerived = await s.request<Node>("$clice/caller", t.marker("derived"), {
        hierarchy: true,
    });
    expect(names(fromDerived)).toContainEqual(["call_base", 1]);
    expect(names(fromDerived)).toContainEqual(["call_qualified", 1]);
    const shallow = await request({ levels: 0 });
    expect(shallow.children).toEqual([]);
    expect(shallow.numChildren).toBe(all.numChildren);
    const deep = await request({ levels: 2 });
    const middle = deep.children.find((c) => c.name === "call_middle");
    if (middle === undefined) throw new Error("missing call_middle");
    expect(names(middle)).toEqual([["outer", 0]]);
    // A returned id is sufficient to expand; no textDocument or position.
    const expanded = (await s.client.sendRequest("$clice/caller", {
        id: middle.id,
        hierarchy: true,
    })) as Node;
    expect(names(expanded)).toEqual(names(middle));
    expect(names(await request({ qualified: false }))).toEqual(names(all));
    expect((await request({ qualified: false })).name).toBe("run");
    const outgoing = await s.request<Node>("$clice/callee", root, { hierarchy: true });
    expect(names(outgoing)).toEqual([
        ["base_leaf", 1],
        ["middle_leaf", 0],
        ["derived_leaf", 2],
    ]);
    const expandedCallee = (await s.client.sendRequest("$clice/callee", {
        id: all.id,
        hierarchy: true,
    })) as Node;
    expect(names(expandedCallee)).toEqual(names(outgoing));
    const directCallee = await s.request<Node>("$clice/callee", root, {
        hierarchy: true,
        callType: 0,
    });
    expect(names(directCallee)).toEqual([["middle_leaf", 0]]);
    expect(await s.request<Location[]>("$clice/callee", root)).toEqual(
        outgoing.children.map((c) => c.location),
    );
    const flat = await s.request<Location[]>("$clice/caller", root, { levels: 2 });
    expect(flat).toHaveLength(5);
    expect(flat).toContainEqual(middle.children[0]?.location);
    const cycle = await s.request<Node>("$clice/caller", t.marker("cycle"), {
        hierarchy: true,
        levels: 3,
    });
    expect(cycle.children[0]?.children[0]?.children[0]?.children).toEqual([]);
    expect(
        await s.client.sendRequest("$clice/caller", {
            id: "42",
            hierarchy: true,
        }),
    ).toBeNull();
    await expect(s.client.sendRequest("$clice/caller", { id: "bad" })).rejects.toThrow();
    await expect(request({ callType: 4 })).rejects.toThrow();
    await expect(request({ levels: -1 })).rejects.toThrow();
});

test("family search continues along the chain", async (t) => {
    const s = await t.start();
    await s.indexed();
    const tree = await s.request<Node>("$clice/caller", t.marker("target"), {
        hierarchy: true,
        levels: 2,
    });
    expect(tree.children.map((c) => c.name)).toEqual(["CallerBase::invoke", "address_user"]);
    const caller = tree.children[0];
    expect(caller?.callType).toBe(0);
    expect(caller?.children.map((c) => [c.name, c.callType])).toEqual([["use_derived", 2]]);
    expect(
        await s.request("$clice/caller", t.at("main.cpp", "struct |CallerBase"), {
            hierarchy: true,
        }),
    ).toBeNull();
});
