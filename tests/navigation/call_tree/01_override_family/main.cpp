void base_leaf() {}
void middle_leaf() {}
void derived_leaf() {}

struct Base {
    virtual void §(base)run() { base_leaf(); }
};
struct Middle : Base {
    void §(middle)run() override { middle_leaf(); }
};
struct Derived : Middle {
    void §(derived)run() override { derived_leaf(); }
};
void call_base(Base& b) { b.run(); }
void call_middle(Middle& m) { m.run(); m.run(); }
void call_derived(Derived& d) { d.run(); }
void call_qualified(Base& b) { b.Base::run(); }
void §(outer)outer(Middle& m) { call_middle(m); }
void §(cycle)cycle() { cycle(); }

void §(target)target() {}
struct CallerBase {
    virtual void invoke() { target(); }
};
struct CallerDerived : CallerBase {
    void invoke() override {}
};
void use_derived(CallerDerived& c) { c.invoke(); }
void address_user() { auto pointer = &target; }
