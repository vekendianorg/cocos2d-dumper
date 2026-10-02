struct A { int x; double y; virtual int f() { return x; } };
struct B : A { char c; int f() override { return c; } };
enum E { P, Q };
int main() { B b; b.x = 1; return b.f() + (int)Q; }
