"use strict";
var __create = Object.create;
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __getProtoOf = Object.getPrototypeOf;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toESM = (mod, isNodeMode, target) => (target = mod != null ? __create(__getProtoOf(mod)) : {}, __copyProps(
  isNodeMode || !mod || !mod.__esModule ? __defProp(target, "default", { value: mod, enumerable: true }) : target,
  mod
));
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

// extension.ts
var extension_exports = {};
__export(extension_exports, {
  activate: () => activate,
  deactivate: () => deactivate
});
module.exports = __toCommonJS(extension_exports);
var vscode = __toESM(require("vscode"));
var path = __toESM(require("path"));
var SURA_KEYWORDS = [
  "is",
  "if",
  "then",
  "else",
  "elif",
  "end",
  "while",
  "do",
  "repeat",
  "for",
  "foreach",
  "in",
  "to",
  "step",
  "break",
  "continue",
  "return",
  "func",
  "class",
  "extends",
  "try",
  "catch",
  "throw",
  "use",
  "new",
  "self",
  "super",
  "true",
  "false",
  "nil",
  "and",
  "or",
  "not"
];
var BUILTIN_FUNCTIONS = {
  "print": { sig: "print(\uAC12, ...)", desc: "\uAC12\uC744 \uCD9C\uB825\uD569\uB2C8\uB2E4", snippet: "print(${1:\uAC12})" },
  "print_n": { sig: "print_n(\uAC12, ...)", desc: "\uC904\uBC14\uAFC8 \uC5C6\uC774 \uCD9C\uB825\uD569\uB2C8\uB2E4", snippet: "print_n(${1:\uAC12})" },
  "input": { sig: "input([\uBA54\uC2DC\uC9C0])", desc: "\uC785\uB825\uC744 \uBC1B\uC544 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: 'input(${1:"\uBA54\uC2DC\uC9C0: "})' },
  "type": { sig: "type(\uAC12)", desc: "\uAC12\uC758 \uD0C0\uC785 \uBB38\uC790\uC5F4\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "type(${1:\uAC12})" },
  "to_num": { sig: "to_num(\uAC12)", desc: "\uC22B\uC790\uB85C \uBCC0\uD658\uD569\uB2C8\uB2E4", snippet: "to_num(${1:\uAC12})" },
  "to_str": { sig: "to_str(\uAC12)", desc: "\uBB38\uC790\uC5F4\uB85C \uBCC0\uD658\uD569\uB2C8\uB2E4", snippet: "to_str(${1:\uAC12})" },
  "abs": { sig: "abs(\uAC12)", desc: "\uC808\uB313\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "abs(${1:\uAC12})" },
  "sqrt": { sig: "sqrt(\uAC12)", desc: "\uC81C\uACF1\uADFC\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "sqrt(${1:\uAC12})" },
  "pow": { sig: "pow(\uBC11, \uC9C0\uC218)", desc: "\uAC70\uB4ED\uC81C\uACF1\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "pow(${1:\uBC11}, ${2:\uC9C0\uC218})" },
  "floor": { sig: "floor(\uAC12)", desc: "\uB0B4\uB9BC\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "floor(${1:\uAC12})" },
  "ceil": { sig: "ceil(\uAC12)", desc: "\uC62C\uB9BC\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "ceil(${1:\uAC12})" },
  "round": { sig: "round(\uAC12)", desc: "\uBC18\uC62C\uB9BC\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "round(${1:\uAC12})" },
  "min": { sig: "min(a, b)", desc: "\uCD5C\uC19F\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "min(${1:a}, ${2:b})" },
  "max": { sig: "max(a, b)", desc: "\uCD5C\uB313\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "max(${1:a}, ${2:b})" },
  "sin": { sig: "sin(\uAC12)", desc: "\uC0AC\uC778\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "sin(${1:\uAC12})" },
  "cos": { sig: "cos(\uAC12)", desc: "\uCF54\uC0AC\uC778\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "cos(${1:\uAC12})" },
  "tan": { sig: "tan(\uAC12)", desc: "\uD0C4\uC820\uD2B8\uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "tan(${1:\uAC12})" },
  "log": { sig: "log(\uAC12)", desc: "\uC790\uC5F0\uB85C\uADF8\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "log(${1:\uAC12})" },
  "inc": { sig: "inc(\uAC12)", desc: "\uAC12 + 1\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "inc(${1:\uAC12})" },
  "dec": { sig: "dec(\uAC12)", desc: "\uAC12 - 1\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "dec(${1:\uAC12})" },
  "rand": { sig: "rand()", desc: "0~1 \uC0AC\uC774 \uC2E4\uC218\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "rand()" },
  "rand_int": { sig: "rand_int(\uCD5C\uC18C, \uCD5C\uB300)", desc: "\uC815\uC218 \uB09C\uC218\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "rand_int(${1:1}, ${2:10})" },
  "rand_float": { sig: "rand_float(\uCD5C\uC18C, \uCD5C\uB300)", desc: "\uC2E4\uC218 \uB09C\uC218\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "rand_float(${1:0.0}, ${2:1.0})" },
  "rand_seed": { sig: "rand_seed(\uC528\uB4DC)", desc: "\uB09C\uC218 \uC528\uB4DC\uB97C \uC124\uC815\uD569\uB2C8\uB2E4", snippet: "rand_seed(${1:42})" },
  "str_len": { sig: "str_len(\uBB38\uC790\uC5F4)", desc: "\uBB38\uC790 \uC218\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "str_len(${1:\uBB38\uC790\uC5F4})" },
  "str_sub": { sig: "str_sub(\uBB38\uC790\uC5F4, \uC2DC\uC791, \uAE38\uC774)", desc: "\uBD80\uBD84 \uBB38\uC790\uC5F4\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "str_sub(${1:\uBB38\uC790\uC5F4}, ${2:0}, ${3:3})" },
  "str_upper": { sig: "str_upper(\uBB38\uC790\uC5F4)", desc: "\uB300\uBB38\uC790\uB85C \uBCC0\uD658\uD569\uB2C8\uB2E4", snippet: "str_upper(${1:\uBB38\uC790\uC5F4})" },
  "str_lower": { sig: "str_lower(\uBB38\uC790\uC5F4)", desc: "\uC18C\uBB38\uC790\uB85C \uBCC0\uD658\uD569\uB2C8\uB2E4", snippet: "str_lower(${1:\uBB38\uC790\uC5F4})" },
  "str_find": { sig: "str_find(\uBB38\uC790\uC5F4, \uAC80\uC0C9\uC5B4)", desc: "\uC704\uCE58 \uC778\uB371\uC2A4\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4 (-1\uC774\uBA74 \uC5C6\uC74C)", snippet: "str_find(${1:\uBB38\uC790\uC5F4}, ${2:\uAC80\uC0C9\uC5B4})" },
  "str_replace": { sig: "str_replace(\uBB38\uC790\uC5F4, \uCC3E\uAE30, \uBC14\uAFB8\uAE30)", desc: "\uCE58\uD658\uB41C \uBB38\uC790\uC5F4\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "str_replace(${1:\uBB38\uC790\uC5F4}, ${2:\uCC3E\uAE30}, ${3:\uBC14\uAFB8\uAE30})" },
  "str_split": { sig: "str_split(\uBB38\uC790\uC5F4, \uAD6C\uBD84\uC790)", desc: "\uBC30\uC5F4\uB85C \uBD84\uB9AC\uD569\uB2C8\uB2E4", snippet: 'str_split(${1:\uBB38\uC790\uC5F4}, ${2:","})' },
  "str_join": { sig: "str_join(\uBC30\uC5F4, \uAD6C\uBD84\uC790)", desc: "\uBC30\uC5F4\uC744 \uD558\uB098\uC758 \uBB38\uC790\uC5F4\uB85C \uD569\uCE69\uB2C8\uB2E4", snippet: 'str_join(${1:\uBC30\uC5F4}, ${2:","})' },
  "str_trim": { sig: "str_trim(\uBB38\uC790\uC5F4)", desc: "\uC55E\uB4A4 \uACF5\uBC31\uC744 \uC81C\uAC70\uD569\uB2C8\uB2E4", snippet: "str_trim(${1:\uBB38\uC790\uC5F4})" },
  "str_starts": { sig: "str_starts(\uBB38\uC790\uC5F4, \uC811\uB450\uC0AC)", desc: "\uC811\uB450\uC0AC\uB85C \uC2DC\uC791\uD558\uBA74 true", snippet: "str_starts(${1:\uBB38\uC790\uC5F4}, ${2:\uC811\uB450\uC0AC})" },
  "str_ends": { sig: "str_ends(\uBB38\uC790\uC5F4, \uC811\uBBF8\uC0AC)", desc: "\uC811\uBBF8\uC0AC\uB85C \uB05D\uB098\uBA74 true", snippet: "str_ends(${1:\uBB38\uC790\uC5F4}, ${2:\uC811\uBBF8\uC0AC})" },
  "str_num": { sig: "str_num(\uBB38\uC790)", desc: "\uBB38\uC790\uC758 ASCII \uCF54\uB4DC\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "str_num(${1:\uBB38\uC790})" },
  "num_str": { sig: "num_str(\uCF54\uB4DC)", desc: "ASCII \uCF54\uB4DC\uB97C \uBB38\uC790\uB85C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "num_str(${1:65})" },
  "arr_new": { sig: "arr_new()", desc: "\uBE48 \uBC30\uC5F4\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "arr_new()" },
  "arr_push": { sig: "arr_push(\uBC30\uC5F4, \uAC12)", desc: "\uBC30\uC5F4 \uB05D\uC5D0 \uAC12\uC744 \uCD94\uAC00\uD569\uB2C8\uB2E4", snippet: "arr_push(${1:\uBC30\uC5F4}, ${2:\uAC12})" },
  "arr_pop": { sig: "arr_pop(\uBC30\uC5F4)", desc: "\uB9C8\uC9C0\uB9C9 \uC694\uC18C\uB97C \uC81C\uAC70\uD558\uACE0 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "arr_pop(${1:\uBC30\uC5F4})" },
  "arr_get": { sig: "arr_get(\uBC30\uC5F4, \uC778\uB371\uC2A4)", desc: "\uC778\uB371\uC2A4\uC758 \uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "arr_get(${1:\uBC30\uC5F4}, ${2:0})" },
  "arr_set": { sig: "arr_set(\uBC30\uC5F4, \uC778\uB371\uC2A4, \uAC12)", desc: "\uC778\uB371\uC2A4\uC758 \uAC12\uC744 \uC124\uC815\uD569\uB2C8\uB2E4", snippet: "arr_set(${1:\uBC30\uC5F4}, ${2:0}, ${3:\uAC12})" },
  "arr_insert": { sig: "arr_insert(\uBC30\uC5F4, \uC778\uB371\uC2A4, \uAC12)", desc: "\uC778\uB371\uC2A4\uC5D0 \uC0BD\uC785\uD569\uB2C8\uB2E4", snippet: "arr_insert(${1:\uBC30\uC5F4}, ${2:0}, ${3:\uAC12})" },
  "arr_remove": { sig: "arr_remove(\uBC30\uC5F4, \uC778\uB371\uC2A4)", desc: "\uC778\uB371\uC2A4\uC758 \uC694\uC18C\uB97C \uC0AD\uC81C\uD569\uB2C8\uB2E4", snippet: "arr_remove(${1:\uBC30\uC5F4}, ${2:0})" },
  "arr_sort": { sig: "arr_sort(\uBC30\uC5F4)", desc: "\uBC30\uC5F4\uC744 \uC815\uB82C\uD569\uB2C8\uB2E4", snippet: "arr_sort(${1:\uBC30\uC5F4})" },
  "arr_reverse": { sig: "arr_reverse(\uBC30\uC5F4)", desc: "\uBC30\uC5F4\uC744 \uB4A4\uC9D1\uC2B5\uB2C8\uB2E4", snippet: "arr_reverse(${1:\uBC30\uC5F4})" },
  "arr_copy": { sig: "arr_copy(\uBC30\uC5F4)", desc: "\uBC30\uC5F4\uC758 \uBCF5\uC0AC\uBCF8\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "arr_copy(${1:\uBC30\uC5F4})" },
  "arr_contains": { sig: "arr_contains(\uBC30\uC5F4, \uAC12)", desc: "\uAC12\uC774 \uC788\uC73C\uBA74 true\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "arr_contains(${1:\uBC30\uC5F4}, ${2:\uAC12})" },
  "arr_clear": { sig: "arr_clear(\uBC30\uC5F4)", desc: "\uBC30\uC5F4\uC744 \uBE44\uC6C1\uB2C8\uB2E4", snippet: "arr_clear(${1:\uBC30\uC5F4})" },
  "arr_sample": { sig: "arr_sample(\uBC30\uC5F4)", desc: "\uC784\uC758\uC758 \uC6D0\uC18C\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "arr_sample(${1:\uBC30\uC5F4})" },
  "arr_shuffle": { sig: "arr_shuffle(\uBC30\uC5F4)", desc: "\uBC30\uC5F4\uC744 \uC11E\uC2B5\uB2C8\uB2E4", snippet: "arr_shuffle(${1:\uBC30\uC5F4})" },
  "len": { sig: "len(\uBC30\uC5F4_\uB610\uB294_\uBB38\uC790\uC5F4)", desc: "\uAE38\uC774\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "len(${1:\uBC30\uC5F4})" },
  "dict_new": { sig: "dict_new()", desc: "\uBE48 \uB515\uC154\uB108\uB9AC\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "dict_new()" },
  "dict_set": { sig: "dict_set(\uB515\uC154\uB108\uB9AC, \uD0A4, \uAC12)", desc: "\uD0A4\uC5D0 \uAC12\uC744 \uC124\uC815\uD569\uB2C8\uB2E4", snippet: 'dict_set(${1:\uB515\uC154\uB108\uB9AC}, ${2:"\uD0A4"}, ${3:\uAC12})' },
  "dict_get": { sig: "dict_get(\uB515\uC154\uB108\uB9AC, \uD0A4)", desc: "\uD0A4\uC758 \uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: 'dict_get(${1:\uB515\uC154\uB108\uB9AC}, ${2:"\uD0A4"})' },
  "dict_has": { sig: "dict_has(\uB515\uC154\uB108\uB9AC, \uD0A4)", desc: "\uD0A4\uAC00 \uC788\uC73C\uBA74 true\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: 'dict_has(${1:\uB515\uC154\uB108\uB9AC}, ${2:"\uD0A4"})' },
  "dict_del": { sig: "dict_del(\uB515\uC154\uB108\uB9AC, \uD0A4)", desc: "\uD0A4\uB97C \uC0AD\uC81C\uD569\uB2C8\uB2E4", snippet: 'dict_del(${1:\uB515\uC154\uB108\uB9AC}, ${2:"\uD0A4"})' },
  "dict_keys": { sig: "dict_keys(\uB515\uC154\uB108\uB9AC)", desc: "\uD0A4 \uBAA9\uB85D \uBC30\uC5F4\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "dict_keys(${1:\uB515\uC154\uB108\uB9AC})" },
  "dict_len": { sig: "dict_len(\uB515\uC154\uB108\uB9AC)", desc: "\uD0A4 \uAC1C\uC218\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "dict_len(${1:\uB515\uC154\uB108\uB9AC})" },
  "file_save": { sig: "file_save(\uACBD\uB85C, \uB0B4\uC6A9)", desc: "\uD30C\uC77C\uC5D0 \uC800\uC7A5\uD569\uB2C8\uB2E4", snippet: 'file_save(${1:"\uD30C\uC77C.txt"}, ${2:\uB0B4\uC6A9})' },
  "file_load": { sig: "file_load(\uACBD\uB85C)", desc: "\uD30C\uC77C \uB0B4\uC6A9\uC744 \uBB38\uC790\uC5F4\uB85C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: 'file_load(${1:"\uD30C\uC77C.txt"})' },
  "file_exists": { sig: "file_exists(\uACBD\uB85C)", desc: "\uD30C\uC77C\uC774 \uC874\uC7AC\uD558\uBA74 true\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: 'file_exists(${1:"\uD30C\uC77C.txt"})' },
  "sleep": { sig: "sleep(\uBC00\uB9AC\uCD08)", desc: "\uC9C0\uC815\uD55C \uC2DC\uAC04\uB9CC\uD07C \uB300\uAE30\uD569\uB2C8\uB2E4", snippet: "sleep(${1:1000})" },
  "time": { sig: "time()", desc: "\uD604\uC7AC Unix \uD0C0\uC784\uC2A4\uD0EC\uD504\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "time()" },
  "exit": { sig: "exit([\uCF54\uB4DC])", desc: "\uD504\uB85C\uADF8\uB7A8\uC744 \uC885\uB8CC\uD569\uB2C8\uB2E4", snippet: "exit(${1:0})" },
  "win_init": { sig: "win_init(\uB108\uBE44, \uB192\uC774, \uC81C\uBAA9)", desc: "\uCC3D\uC744 \uB9CC\uB4ED\uB2C8\uB2E4", snippet: 'win_init(${1:800}, ${2:600}, ${3:"\uCC3D \uC81C\uBAA9"})' },
  "win_clear": { sig: "win_clear(r, g, b)", desc: "\uD654\uBA74\uC744 \uC9C0\uC6C1\uB2C8\uB2E4", snippet: "win_clear(${1:0}, ${2:0}, ${3:0})" },
  "win_draw": { sig: "win_draw()", desc: "\uD654\uBA74\uC744 \uAC31\uC2E0\uD569\uB2C8\uB2E4", snippet: "win_draw()" },
  "win_close": { sig: "win_close()", desc: "\uCC3D\uC744 \uB2EB\uC2B5\uB2C8\uB2E4", snippet: "win_close()" },
  "win_open": { sig: "win_open()", desc: "\uCC3D\uC774 \uC5F4\uB824\uC788\uC73C\uBA74 true", snippet: "win_open()" },
  "mouse_pos": { sig: "mouse_pos()", desc: "\uB9C8\uC6B0\uC2A4 \uC704\uCE58 [x, y]\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: "mouse_pos()" },
  "mouse_down": { sig: "mouse_down(\uBC84\uD2BC)", desc: "\uB9C8\uC6B0\uC2A4 \uBC84\uD2BC\uC774 \uB20C\uB9AC\uBA74 true", snippet: "mouse_down(${1:0})" },
  "key_down": { sig: "key_down(\uD0A4)", desc: "\uD0A4\uAC00 \uB20C\uB9AC\uBA74 true\uB97C \uBC18\uD658\uD569\uB2C8\uB2E4", snippet: 'key_down(${1:"Space"})' }
};
var KEYWORD_DOCS = {
  "is": '`\uBCC0\uC218 is \uAC12` \u2014 \uBCC0\uC218\uC5D0 \uAC12\uC744 \uB300\uC785\uD569\uB2C8\uB2E4\n\n```\nx is 10\ns is str_upper("hello")\n```',
  "if": "`if \uC870\uAC74 then ... end` \u2014 \uC870\uAC74\uBB38\n\n```\nif x > 5 then\n    print(x)\nend\n```",
  "elif": '`elif \uC870\uAC74 then` \u2014 \uCD94\uAC00 \uC870\uAC74 (if \uC548\uC5D0\uC11C \uC0AC\uC6A9)\n\n```\nif x == 1 then\n    print("\uD558\uB098")\nelif x == 2 then\n    print("\uB458")\nend\n```',
  "else": "`else` \u2014 \uC870\uAC74\uC774 \uAC70\uC9D3\uC77C \uB54C \uC2E4\uD589",
  "while": "`while \uC870\uAC74 do ... end` \u2014 \uBC18\uBCF5\uBB38\n\n```\nwhile i < 10 do\n    i is inc(i)\nend\n```",
  "for": "`for i in 1 to 10 do ... end` \u2014 \uBC94\uC704 \uBC18\uBCF5\n\n```\nfor i in 1 to 5 do\n    print(i)\nend\n```",
  "foreach": "`foreach item in \uBC30\uC5F4 do ... end` \u2014 \uBC30\uC5F4 \uC21C\uD68C\n\n```\nforeach v in arr do\n    print(v)\nend\n```",
  "func": "`func \uC774\uB984(\uC778\uC790) do ... end` \u2014 \uD568\uC218 \uC815\uC758\n\n```\nfunc add(a, b) do\n    return a + b\nend\n```",
  "class": '`class \uC774\uB984 do ... end` \u2014 \uD074\uB798\uC2A4 \uC815\uC758\n\n```\nclass Dog do\n    name is "Rex"\n    func bark() do\n        print("Woof!")\n    end\nend\n```',
  "return": "`return \uAC12` \u2014 \uD568\uC218\uC5D0\uC11C \uAC12\uC744 \uBC18\uD658\uD569\uB2C8\uB2E4",
  "break": "`break` \u2014 \uBC18\uBCF5\uBB38\uC744 \uC989\uC2DC \uD0C8\uCD9C\uD569\uB2C8\uB2E4",
  "continue": "`continue` \u2014 \uB2E4\uC74C \uBC18\uBCF5\uC73C\uB85C \uB118\uC5B4\uAC11\uB2C8\uB2E4",
  "try": "`try ... catch \uBCC0\uC218 ... end` \u2014 \uC608\uC678 \uCC98\uB9AC\n\n```\ntry\n    result is 1 / 0\ncatch err\n    print(err)\nend\n```",
  "new": '`new \uD074\uB798\uC2A4(\uC778\uC790)` \u2014 \uC778\uC2A4\uD134\uC2A4\uB97C \uC0DD\uC131\uD569\uB2C8\uB2E4\n\n```\ndog is new Dog("Rex")\n```',
  "use": "`use \uB77C\uC774\uBE0C\uB7EC\uB9AC` \u2014 \uBAA8\uB4C8\uC744 \uBD88\uB7EC\uC635\uB2C8\uB2E4\n\n```\nuse math\n```",
  "self": "`self` \u2014 \uD604\uC7AC \uC778\uC2A4\uD134\uC2A4\uB97C \uAC00\uB9AC\uD0B5\uB2C8\uB2E4",
  "super": "`super.\uBA54\uC11C\uB4DC(\uC778\uC790)` \u2014 \uBD80\uBAA8 \uD074\uB798\uC2A4\uC758 \uBA54\uC11C\uB4DC\uB97C \uD638\uCD9C\uD569\uB2C8\uB2E4",
  "nil": "`nil` \u2014 \uAC12 \uC5C6\uC74C (null)",
  "true": "`true` \u2014 \uCC38",
  "false": "`false` \u2014 \uAC70\uC9D3",
  "and": "`A and B` \u2014 \uB17C\uB9AC AND",
  "or": "`A or B` \u2014 \uB17C\uB9AC OR",
  "not": "`not A` \u2014 \uB17C\uB9AC NOT"
};
var SuraCompletionProvider = class {
  provideCompletionItems(document, position) {
    const lineText = document.lineAt(position.line).text;
    const wordRange = document.getWordRangeAtPosition(position);
    const word = wordRange ? document.getText(wordRange) : "";
    const completions = [];
    if (/^\s*use\s+\w*$/.test(lineText)) {
      for (const lib of ["math", "game", "string", "system", "time"]) {
        if (lib.startsWith(word)) {
          const item = new vscode.CompletionItem(lib, vscode.CompletionItemKind.Module);
          item.detail = "Sura \uD45C\uC900 \uB77C\uC774\uBE0C\uB7EC\uB9AC";
          completions.push(item);
        }
      }
      return completions;
    }
    for (const kw of SURA_KEYWORDS) {
      if (kw.startsWith(word)) {
        const item = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
        item.detail = "Sura \uD0A4\uC6CC\uB4DC";
        if (kw in KEYWORD_DOCS) {
          item.documentation = new vscode.MarkdownString(KEYWORD_DOCS[kw]);
        }
        completions.push(item);
      }
    }
    for (const [name, info] of Object.entries(BUILTIN_FUNCTIONS)) {
      if (name.startsWith(word)) {
        const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
        item.detail = info.sig;
        item.documentation = new vscode.MarkdownString(
          `\`\`\`sura
${info.sig}
\`\`\`

${info.desc}`
        );
        item.insertText = new vscode.SnippetString(info.snippet);
        item.sortText = "0_" + name;
        completions.push(item);
      }
    }
    return completions;
  }
};
var SuraSignatureHelpProvider = class {
  provideSignatureHelp(document, position) {
    const lineText = document.lineAt(position.line).text.substring(0, position.character);
    let depth = 0;
    let funcEnd = -1;
    for (let i = lineText.length - 1; i >= 0; i--) {
      if (lineText[i] === ")")
        depth++;
      else if (lineText[i] === "(") {
        if (depth === 0) {
          funcEnd = i;
          break;
        }
        depth--;
      }
    }
    if (funcEnd < 0)
      return void 0;
    const before = lineText.substring(0, funcEnd);
    const match = before.match(/([a-zA-Z_][a-zA-Z0-9_]*)$/);
    if (!match)
      return void 0;
    const funcName = match[1];
    if (!(funcName in BUILTIN_FUNCTIONS))
      return void 0;
    const info = BUILTIN_FUNCTIONS[funcName];
    const argsStr = lineText.substring(funcEnd + 1);
    let argIndex = 0;
    let d = 0;
    for (const ch of argsStr) {
      if (ch === "(" || ch === "[")
        d++;
      else if (ch === ")" || ch === "]")
        d--;
      else if (ch === "," && d === 0)
        argIndex++;
    }
    const sigInfo = new vscode.SignatureInformation(info.sig, new vscode.MarkdownString(info.desc));
    const paramMatch = info.sig.match(/\(([^)]*)\)/);
    if (paramMatch && paramMatch[1]) {
      const params = paramMatch[1].split(",").map((p) => p.trim());
      for (const p of params) {
        sigInfo.parameters.push(new vscode.ParameterInformation(p));
      }
    }
    const help = new vscode.SignatureHelp();
    help.signatures = [sigInfo];
    help.activeSignature = 0;
    help.activeParameter = Math.min(argIndex, sigInfo.parameters.length - 1);
    return help;
  }
};
var SuraHoverProvider = class {
  provideHover(document, position) {
    const wordRange = document.getWordRangeAtPosition(position);
    if (!wordRange)
      return void 0;
    const word = document.getText(wordRange);
    if (word in BUILTIN_FUNCTIONS) {
      const info = BUILTIN_FUNCTIONS[word];
      const md = new vscode.MarkdownString(
        `\`\`\`sura
${info.sig}
\`\`\`

${info.desc}`
      );
      return new vscode.Hover(md, wordRange);
    }
    if (word in KEYWORD_DOCS) {
      return new vscode.Hover(new vscode.MarkdownString(KEYWORD_DOCS[word]), wordRange);
    }
    for (let i = 0; i < document.lineCount; i++) {
      const line = document.lineAt(i).text;
      const m = line.match(new RegExp(`func\\s+${word}\\s*\\(([^)]*)\\)`));
      if (m) {
        const params = m[1] || "";
        const md = new vscode.MarkdownString(
          `\`\`\`sura
func ${word}(${params})
\`\`\`

\uC0AC\uC6A9\uC790 \uC815\uC758 \uD568\uC218 (${i + 1}\uBC88 \uC904)`
        );
        return new vscode.Hover(md, wordRange);
      }
    }
    return void 0;
  }
};
var SuraDefinitionProvider = class {
  provideDefinition(document, position) {
    const wordRange = document.getWordRangeAtPosition(position);
    if (!wordRange)
      return void 0;
    const word = document.getText(wordRange);
    for (let i = 0; i < document.lineCount; i++) {
      const line = document.lineAt(i).text;
      if (line.match(new RegExp(`^\\s*func\\s+${word}\\s*(\\(|do)`)) || line.match(new RegExp(`^\\s*class\\s+${word}\\s*(extends|do)`))) {
        return new vscode.Location(document.uri, new vscode.Position(i, 0));
      }
    }
    return void 0;
  }
};
var SuraFormattingProvider = class {
  provideDocumentFormattingEdits(document) {
    const edits = [];
    let indent = 0;
    const TAB = "    ";
    const DECREASE = /^\s*(end|else|elif|catch)\b/;
    const INCREASE = /\b(then|do)\s*$/;
    const NEUTRAL = /^\s*(else|elif|catch)\b/;
    for (let i = 0; i < document.lineCount; i++) {
      const line = document.lineAt(i);
      const text = line.text.trim();
      if (text === "" || text.startsWith("//") || text.startsWith("#"))
        continue;
      if (DECREASE.test(text))
        indent = Math.max(0, indent - 1);
      const expected = TAB.repeat(indent);
      const actual = line.text.substring(0, line.text.length - line.text.trimStart().length);
      if (actual !== expected) {
        edits.push(vscode.TextEdit.replace(
          new vscode.Range(i, 0, i, actual.length),
          expected
        ));
      }
      if (INCREASE.test(text) && !NEUTRAL.test(text))
        indent++;
    }
    return edits;
  }
};
var SuraDiagnosticProvider = class {
  constructor(context) {
    this.collection = vscode.languages.createDiagnosticCollection("sura");
    context.subscriptions.push(this.collection);
  }
  check(document) {
    if (document.languageId !== "sura")
      return;
    const diags = [];
    const blockStack = [];
    for (let i = 0; i < document.lineCount; i++) {
      const raw = document.lineAt(i).text;
      const commentIdx = this.commentIndex(raw);
      const text = commentIdx >= 0 ? raw.substring(0, commentIdx).trim() : raw.trim();
      if (text === "")
        continue;
      const counts = this.countBrackets(text);
      if (counts.open !== counts.close) {
        diags.push(new vscode.Diagnostic(
          new vscode.Range(i, 0, i, raw.length),
          `\uAD04\uD638\uAC00 \uB9DE\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4 (\uC5F4\uB9BC ${counts.open}\uAC1C, \uB2EB\uD798 ${counts.close}\uAC1C)`,
          vscode.DiagnosticSeverity.Warning
        ));
      }
      if (/\b(if|while|for|foreach|func|class|try)\b/.test(text) && /\b(then|do)\b/.test(text)) {
        blockStack.push({ keyword: text.match(/\b(if|while|for|foreach|func|class|try)\b/)[0], line: i });
      }
      if (/^\s*(else|elif|catch)\b/.test(raw)) {
        if (blockStack.length === 0) {
          diags.push(new vscode.Diagnostic(
            new vscode.Range(i, 0, i, raw.length),
            `\uB300\uC751\uD558\uB294 if/try\uAC00 \uC5C6\uC2B5\uB2C8\uB2E4`,
            vscode.DiagnosticSeverity.Warning
          ));
        }
      }
      if (/^\s*end\b/.test(text)) {
        if (blockStack.length > 0)
          blockStack.pop();
        else {
          diags.push(new vscode.Diagnostic(
            new vscode.Range(i, 0, i, raw.length),
            "\uB300\uC751\uD558\uB294 \uBE14\uB85D \uC2DC\uC791\uC774 \uC5C6\uB294 end\uC785\uB2C8\uB2E4",
            vscode.DiagnosticSeverity.Warning
          ));
        }
      }
    }
    for (const b of blockStack) {
      diags.push(new vscode.Diagnostic(
        new vscode.Range(b.line, 0, b.line, document.lineAt(b.line).text.length),
        `'${b.keyword}' \uBE14\uB85D\uC774 end\uB85C \uB2EB\uD788\uC9C0 \uC54A\uC558\uC2B5\uB2C8\uB2E4`,
        vscode.DiagnosticSeverity.Warning
      ));
    }
    this.collection.set(document.uri, diags);
  }
  commentIndex(text) {
    let inStr = false;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === '"' && (i === 0 || text[i - 1] !== "\\"))
        inStr = !inStr;
      if (!inStr) {
        if (text[i] === "#")
          return i;
        if (text[i] === "/" && text[i + 1] === "/")
          return i;
      }
    }
    return -1;
  }
  countBrackets(text) {
    let open = 0, close = 0, inStr = false;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === '"' && (i === 0 || text[i - 1] !== "\\")) {
        inStr = !inStr;
        continue;
      }
      if (inStr)
        continue;
      if (text[i] === "(")
        open++;
      else if (text[i] === ")")
        close++;
    }
    return { open, close };
  }
};
var SuraSnippetProvider = class {
  provideCompletionItems() {
    const snippets = [];
    const add = (label, body, doc) => {
      const item = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
      item.insertText = new vscode.SnippetString(body);
      item.documentation = doc;
      item.sortText = "9_" + label;
      snippets.push(item);
    };
    add("if", "if ${1:\uC870\uAC74} then\n	$2\nend", "\uC870\uAC74\uBB38");
    add("ife", "if ${1:\uC870\uAC74} then\n	$2\nelse\n	$3\nend", "\uC870\uAC74\uBB38 (else \uD3EC\uD568)");
    add("elif", "elif ${1:\uC870\uAC74} then\n	$2", "elif \uBD84\uAE30");
    add("while", "while ${1:\uC870\uAC74} do\n	$2\nend", "\uBC18\uBCF5\uBB38");
    add("for", "for ${1:i} in ${2:1} to ${3:10} do\n	$4\nend", "\uBC94\uC704 \uBC18\uBCF5");
    add("foreach", "foreach ${1:item} in ${2:arr} do\n	$3\nend", "\uBC30\uC5F4 \uC21C\uD68C");
    add("func", "func ${1:\uD568\uC218\uBA85}(${2:\uC778\uC790}) do\n	${3:return nil}\nend", "\uD568\uC218 \uC815\uC758");
    add("class", "class ${1:\uD074\uB798\uC2A4\uBA85} do\n	func init(${2:\uC778\uC790}) do\n		$3\n	end\nend", "\uD074\uB798\uC2A4 \uC815\uC758");
    add("try", "try\n	$1\ncatch ${2:err}\n	print(${2:err})\nend", "\uC608\uC678 \uCC98\uB9AC");
    add("main", "// main\n${1:x} is ${2:0}\n$3", "\uBA54\uC778 \uCF54\uB4DC");
    return snippets;
  }
};
function activate(context) {
  const SEL = { language: "sura" };
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(SEL, new SuraCompletionProvider(), ".", "_", "("),
    vscode.languages.registerCompletionItemProvider(SEL, new SuraSnippetProvider()),
    vscode.languages.registerSignatureHelpProvider(SEL, new SuraSignatureHelpProvider(), "(", ","),
    vscode.languages.registerHoverProvider(SEL, new SuraHoverProvider()),
    vscode.languages.registerDefinitionProvider(SEL, new SuraDefinitionProvider()),
    vscode.languages.registerDocumentFormattingEditProvider(SEL, new SuraFormattingProvider())
  );
  const diag = new SuraDiagnosticProvider(context);
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((d) => diag.check(d)),
    vscode.workspace.onDidChangeTextDocument((e) => diag.check(e.document)),
    vscode.workspace.onDidSaveTextDocument((d) => diag.check(d))
  );
  vscode.workspace.textDocuments.forEach((d) => diag.check(d));
  context.subscriptions.push(
    vscode.commands.registerCommand("sura.runFile", () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== "sura")
        return;
      const filePath = editor.document.fileName;
      editor.document.save();
      const config = vscode.workspace.getConfiguration("sura");
      const enginePath = config.get("enginePath") || "SuraEngine2.exe";
      const cwd = path.dirname(filePath);
      const engine = path.isAbsolute(enginePath) ? enginePath : path.join(cwd, enginePath);
      let term = vscode.window.terminals.find((t) => t.name === "Sura");
      if (!term)
        term = vscode.window.createTerminal("Sura");
      term.show();
      term.sendText(`"${engine}" "${filePath}"`);
    })
  );
  context.subscriptions.push(
    vscode.commands.registerCommand("sura.runJIT", () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== "sura")
        return;
      const filePath = editor.document.fileName;
      editor.document.save();
      const cwd = path.dirname(filePath);
      const engine = path.join(cwd, "SuraJIT.exe");
      let term = vscode.window.terminals.find((t) => t.name === "Sura JIT");
      if (!term)
        term = vscode.window.createTerminal("Sura JIT");
      term.show();
      term.sendText(`"${engine}" "${filePath}"`);
    })
  );
  context.subscriptions.push(
    vscode.commands.registerCommand("sura.format", () => {
      vscode.commands.executeCommand("editor.action.formatDocument");
    })
  );
}
function deactivate() {
}
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  activate,
  deactivate
});
