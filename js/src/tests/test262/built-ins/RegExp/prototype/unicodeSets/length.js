// Copyright (C) 2022 Mathias Bynens, Ron Buckton, and André Bargull. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-get-regexp.prototype.unicodesets
description: >
  get RegExp.prototype.unicodeSets.length is 0.
info: |
  get RegExp.prototype.unicodeSets

  17 ECMAScript Standard Built-in Objects:
    Every built-in Function object, including constructors, has a length
    property whose value is an integer. Unless otherwise specified, this
    value is equal to the largest number of named arguments shown in the
    subclause headings for the function description, including optional
    parameters. However, rest parameters shown using the form “...name”
    are not included in the default argument count.

    Unless otherwise specified, the length property of a built-in Function
    object has the attributes { [[Writable]]: false, [[Enumerable]]: false,
    [[Configurable]]: true }.
includes: [propertyHelper.js]
features: [regexp-v-flag]
---*/

var desc = Object.getOwnPropertyDescriptor(RegExp.prototype, "unicodeSets");

assert.sameValue(desc.get.length, 0);

verifyProperty(desc.get, "length", {
  enumerable: false,
  writable: false,
  configurable: true,
});

reportCompare(0, 0);
