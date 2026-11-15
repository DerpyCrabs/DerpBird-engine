// A constructor needing a function environment takes the construct stub's runtime-prepared path.
// The stub enters its compiled code directly, so that frame must retain the code while it runs.
function checkSlowConstructCodeLifetime() {
    function Constructor(value) {
        this.read = () => value;
        this.compiled = jit.inCompiledCode();
        this.value = value + 1;
    }
    function construct(C, value) {
        return new C(value);
    }
    jit.prepare(Constructor);
    jit.prepare(construct);
    for (let i = 0; i < 10; ++i) construct(Constructor, i);
    expect(jit.compile(Constructor)).toBe(jit.enabled);
    expect(jit.compile(construct)).toBe(jit.enabled);
    const result = construct(Constructor, 41);
    if (!jit.forcesExits) expect(result.compiled).toBe(jit.enabled);
    expect(result.read()).toBe(41);
    expect(result.value).toBe(42);
}
jit.neverCompile(checkSlowConstructCodeLifetime);
test("runtime-prepared constructor frames retain their compiled code", checkSlowConstructCodeLifetime);
