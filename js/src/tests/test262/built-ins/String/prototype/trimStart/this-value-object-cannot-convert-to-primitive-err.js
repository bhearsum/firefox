// Copyright (C) 2017 Valerie Young. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-string.prototype.trimstart
description: >
  This value is an object which cannot be converted to a primitive
info: |
  Runtime Semantics: TrimString ( string, where )
  1. Let str be ? RequireObjectCoercible(string).
  2. Let S be ? ToString(str).
   ...

  ToString ( argument )
  If argument is Object:
    1. Let primValue be ? ToPrimitive(argument, hint String).
   ...

  ToPrimitive ( input [, PreferredType ])
   ...
    b. Else if PreferredType is hint String, let hint be "string".
   ...
    d. Let exoticToPrim be ? GetMethod(input, @@toPrimitive)
    e. If exoticToPrim is not undefined, then
      i. Let result be ? Call(exoticToPrim, input, « hint »).
      ii. If Type(result) is not Object, return result.
      iii. Throw a TypeError exception.
    f. If hint is "default", set hint to "number".
    g. Return ? OrdinaryToPrimitive(input, hint).
   ...

  OrdinaryToPrimitive( O, hint )
   ...
    3. If hint is "string", then
      a. Let methodNames be « "toString", "valueOf" ».
   ...
    5. For each name in methodNames in List order, do
      a. Let method be ? Get(O, name).
      b. If IsCallable(method) is true, then
        i. Let result be ? Call(method, O).
        ii. If Type(result) is not Object, return result.
    6. Throw a TypeError exception.
features: [string-trimming, String.prototype.trimStart, Symbol.toPrimitive]
---*/

assert.sameValue(typeof String.prototype.trimStart, "function");

var thisVal = {
  [Symbol.toPrimitive]: undefined,
  toString: undefined,
  valueOf: undefined,
};

// If trimStart is called on an object with neither Symbol.toPrimitive, toString
// nor valueOf defined, then a TypeError exception should be thrown.
assert.throws(
  TypeError,
  function() {
    String.prototype.trimStart.call(thisVal);
  }
);

reportCompare(0, 0);
