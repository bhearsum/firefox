// -0 is a valid value with which to create an i31ref, but because it
// roundtrips to +0, it wrecks a lot of our WASM tests (which use Object.is for
// comparison). Therefore we don't include -0 in the main list, and deal with
// that complexity in this file specifically.
WasmI31refValues.push(-0);

let InvalidI31Values = [
  null,
  Number.EPSILON,
  Number.MAX_SAFE_INTEGER,
  Number.MIN_SAFE_INTEGER,
  Number.MIN_VALUE,
  Number.MAX_VALUE,
  Infinity,
  -Infinity,
  Number.NaN,
  // Number objects are not coerced
  ...WasmI31refValues.map(n => new Number(n)),
  // Non-integers are not valid
  ...WasmI31refValues.map(n => n + 0.1),
  ...WasmI31refValues.map(n => n + 0.5),
  ...WasmI31refValues.map(n => n + 0.9)
];

// Return an equivalent JS number for if a JS number is converted to i31ref
// and then zero extended back to 32-bits.
function valueAsI31GetU(value) {
  // Zero extending will drop the sign bit, if any
  return value & 0x7fffffff;
}

let identity = (n) => n;

let {
  castFromAnyref,
  castFromExternref,
  refI31,
  refI31Identity,
  i31GetU,
  i31GetS,
  i31EqualsI31,
  i31EqualsEq
} = wasmEvalText(`(module
  (func $identity (import "" "identity") (param anyref) (result anyref))

  (func (export "castFromAnyref") (param anyref) (result i32)
    local.get 0
    ref.test (ref i31)
  )
  (func (export "castFromExternref") (param externref) (result i32)
    local.get 0
    any.convert_extern
    ref.test (ref i31)
  )
  (func (export "refI31") (param i32) (result anyref)
    local.get 0
    ref.i31
  )
  (func (export "refI31Identity") (param i32) (result anyref)
    local.get 0
    ref.i31
    call $identity
  )
  (func (export "i31GetU") (param i32) (result i32)
    local.get 0
    ref.i31
    i31.get_u
  )
  (func (export "i31GetS") (param i32) (result i32)
    local.get 0
    ref.i31
    i31.get_s
  )
  (func (export "i31EqualsI31") (param i32) (param i32) (result i32)
    (ref.eq
      (ref.i31 local.get 0)
      (ref.i31 local.get 1)
    )
  )
  (func (export "i31EqualsEq") (param i32) (param eqref) (result i32)
    (ref.eq
      (ref.i31 local.get 0)
      local.get 1
    )
  )
)`, {"": {identity}}).exports;

// Test that wasm will represent JS number values that are 31-bit integers as
// an i31ref
for (let i of WasmI31refValues) {
  assertEq(castFromAnyref(i), 1);
  assertEq(castFromExternref(i), 1);
}

// Test that wasm will not represent a JS value that is not a 31-bit number as
// an i31ref
for (let i of InvalidI31Values) {
  assertEq(castFromAnyref(i), 0);
  assertEq(castFromExternref(i), 0);
}

// Test that we can roundtrip 31-bit integers through the i31ref type
// faithfully.
for (let i of WasmI31refValues) {
  assertEq(refI31(i), Object.is(i, -0) ? 0 : i);
  assertEq(refI31Identity(i), Object.is(i, -0) ? 0 : i);
  assertEq(i31GetU(i), valueAsI31GetU(i));
  assertEq(i31GetS(i), Object.is(i, -0) ? 0 : i);
}

// Test that i31ref values are truncated when given a 32-bit value
for (let i of WasmI31refValues) {
  let adjusted = i | 0x80000000;
  assertEq(refI31(adjusted), Object.is(i, -0) ? 0 : i);
}

// Test that comparing identical i31 values works
for (let a of WasmI31refValues) {
  for (let b of WasmI31refValues) {
    assertEq(!!i31EqualsI31(a, b), a === b);
  }
}

// Test that an i31ref is never mistaken for a different kind of reference
for (let a of WasmI31refValues) {
  for (let b of WasmEqrefValues) {
    assertEq(!!i31EqualsEq(a, b), a === b);
  }
}

// Test that i32 values get wrapped correctly
const bigI32Tests = [
  {
    input: MaxI31refValue + 1,
    expected: MinI31refValue,
  },
  {
    input: MinI31refValue - 1,
    expected: MaxI31refValue,
  },
]
for (const {input, expected} of bigI32Tests) {
  const { get, getElem } = wasmEvalText(`(module
    (func (export "get") (param i32) (result i32)
      (i31.get_s (ref.i31 (local.get 0)))
    )

    (table i31ref (elem (item (ref.i31 (i32.const ${input})))))
    (func (export "getElem") (result i32)
      (i31.get_s (table.get 0 (i32.const 0)))
    )
  )`).exports;
  assertEq(get(input), expected);
  assertEq(getElem(), expected);
}

// Test that (ref.i31 (i32 const value)) optimization is correct
for (let value of WasmI31refValues) {
  let {compare} = wasmEvalText(`(module
  (func $innerCompare (param i32) (param i31ref) (result i32)
    (ref.eq
      (ref.i31 local.get 0)
      local.get 1
    )
  )
  (func (export "compare") (result i32)
    i32.const ${value}
    (ref.i31 i32.const ${value})
    call $innerCompare
  )
)`).exports;
  assertEq(compare(value), 1);
}

const { i31GetU_null, i31GetS_null } = wasmEvalText(`(module
  (func (export "i31GetU_null") (result i32)
    ref.null i31
    i31.get_u
  )
  (func (export "i31GetS_null") (result i32)
    ref.null i31
    i31.get_s
  )
)`).exports;

assertErrorMessage(() => i31GetU_null(), WebAssembly.RuntimeError, /dereferencing null pointer/);
assertErrorMessage(() => i31GetS_null(), WebAssembly.RuntimeError, /dereferencing null pointer/);
