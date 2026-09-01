import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind
} from 'vscode-languageclient/node';

type BuiltinInfo = {
  signature: string;
  description: string;
  snippet: string;
};

type ProjectSymbolInfo = {
  name: string;
  kind: vscode.CompletionItemKind;
  detail: string;
  snippet?: string;
};

const KEYWORDS = [
  'is', 'if', 'then', 'else', 'elif', 'end',
  'while', 'do', 'repeat', 'for', 'foreach', 'in', 'to', 'step',
  'when', 'case', 'default',
  'break', 'continue', 'return',
  'func', 'class', 'extends', 'enum',
  'try', 'catch', 'throw',
  'import', 'use', 'new', 'self', 'super',
  'true', 'false', 'nil',
  'and', 'or', 'not'
];

const MODULES = [
  'array', 'math', 'path', 'string', 'os', 'cli', 'json', 'fs',
  'regex', 'datetime', 'crypto', 'db', 'log', 'console', 'http', 'async',
  'test', 'random', 'python', 'ffi', 'plugin', 'vector', 'graphics3d', 'rag', 'tensor', 'nn', 'ai', 'autograd', 'tokenizer', 'dataset', 'media', 'stream', 'tool', 'llm'
];

const COMPLETION_TRIGGER_CHARS = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_ '.split('');
const IDENTIFIER_PATTERN = String.raw`[\p{L}_][\p{L}\p{N}_]*`;

const BUILTINS: Record<string, BuiltinInfo> = {
  print: { signature: 'print value', description: 'Writes values with a newline.', snippet: 'print ${1:value}' },
  print_n: { signature: 'print_n value', description: 'Writes values without a newline.', snippet: 'print_n ${1:value}' },
  input: { signature: 'input([prompt])', description: 'Reads one line from stdin.', snippet: 'input(${1:"prompt: "})' },
  type: { signature: 'type(value)', description: 'Returns the runtime type name.', snippet: 'type(${1:value})' },
  clock: { signature: 'clock()', description: 'Returns a monotonic timestamp in seconds.', snippet: 'clock()' },
  sleep: { signature: 'sleep(milliseconds)', description: 'Blocks the current script for the given number of milliseconds.', snippet: 'sleep(${1:16})' },
  cls: { signature: 'cls()', description: 'Clears an interactive terminal. Alias for console_clear().', snippet: 'cls' },
  silent: { signature: 'silent([on|off])', description: 'Compatibility no-op for old console examples.', snippet: 'silent ${1:on}' },
  key_down: { signature: 'key_down(key)', description: 'Returns true while a keyboard key is currently pressed.', snippet: 'key_down(${1:"space"})' },
  readkey: { signature: 'readkey()', description: 'Reads one key and blocks until input is available.', snippet: 'readkey(${1})' },
  readkey_timeout: { signature: 'readkey_timeout(ms)', description: 'Reads one key without blocking longer than the timeout.', snippet: 'readkey_timeout(${1:16})' },
  win_init: { signature: 'win_init(width, height, title)', description: 'Opens a native Sura drawing window.', snippet: 'win_init(${1:800}, ${2:600}, ${3:"Sura"})' },
  win_clear: { signature: 'win_clear(r, g, b)', description: 'Clears the Sura drawing window.', snippet: 'win_clear(${1:0}, ${2:0}, ${3:0})' },
  win_rect: { signature: 'win_rect(x, y, width, height, r, g, b)', description: 'Draws a filled rectangle in the Sura drawing window.', snippet: 'win_rect(${1:20}, ${2:20}, ${3:80}, ${4:40}, ${5:255}, ${6:255}, ${7:255})' },
  win_circle: { signature: 'win_circle(x, y, radius, r, g, b)', description: 'Draws a filled circle in the Sura drawing window.', snippet: 'win_circle(${1:100}, ${2:100}, ${3:20}, ${4:255}, ${5:255}, ${6:255})' },
  win_line: { signature: 'win_line(x1, y1, x2, y2, r, g, b)', description: 'Draws a line in the Sura drawing window.', snippet: 'win_line(${1:0}, ${2:0}, ${3:100}, ${4:100}, ${5:255}, ${6:255}, ${7:255})' },
  win_text: { signature: 'win_text(text, x, y, r, g, b)', description: 'Draws UTF-8 text in the Sura drawing window.', snippet: 'win_text(${1:"Sura"}, ${2:20}, ${3:20}, ${4:255}, ${5:255}, ${6:255})' },
  win_update: { signature: 'win_update()', description: 'Pumps window events, presents a frame, and returns whether the drawing window is open.', snippet: 'win_update()' },
  win_poll: { signature: 'win_poll()', description: 'Pumps window events without presenting a frame and returns whether the drawing window is open.', snippet: 'win_poll()' },
  win_focus: { signature: 'win_focus()', description: 'Requests keyboard focus for the Sura drawing window.', snippet: 'win_focus()' },
  win_close: { signature: 'win_close()', description: 'Closes the Sura drawing window.', snippet: 'win_close()' },
  mouse_pos: { signature: 'mouse_pos()', description: 'Returns the mouse position as {x, y}.', snippet: 'mouse_pos()' },
  mouse_down: { signature: 'mouse_down(button)', description: 'Returns true while a mouse button is pressed.', snippet: 'mouse_down(${1:"left"})' },
  grid_init: { signature: 'grid_init(width, height)', description: 'Creates a terminal grid buffer for text games.', snippet: 'grid_init(${1:80}, ${2:25})' },
  grid_clear: { signature: 'grid_clear()', description: 'Clears the terminal grid buffer or the terminal screen.', snippet: 'grid_clear()' },
  grid_set: { signature: 'grid_set(x, y, char, [color])', description: 'Writes one terminal grid cell, optionally with color.', snippet: 'grid_set(${1:0}, ${2:0}, ${3:"@"}, ${4:"green"})' },
  grid_draw: { signature: 'grid_draw()', description: 'Draws the terminal grid buffer to the console.', snippet: 'grid_draw()' },

  sqrt: { signature: 'sqrt(number)', description: 'Square root.', snippet: 'sqrt(${1:number})' },
  sin: { signature: 'sin(number)', description: 'Sine.', snippet: 'sin(${1:number})' },
  cos: { signature: 'cos(number)', description: 'Cosine.', snippet: 'cos(${1:number})' },
  tan: { signature: 'tan(number)', description: 'Tangent.', snippet: 'tan(${1:number})' },
  floor: { signature: 'floor(number)', description: 'Rounds down.', snippet: 'floor(${1:number})' },
  ceil: { signature: 'ceil(number)', description: 'Rounds up.', snippet: 'ceil(${1:number})' },
  round: { signature: 'round(number)', description: 'Rounds to the nearest integer.', snippet: 'round(${1:number})' },
  abs: { signature: 'abs(number)', description: 'Absolute value.', snippet: 'abs(${1:number})' },
  sign: { signature: 'sign(number)', description: 'Returns -1, 0, or 1 for the numeric sign.', snippet: 'sign(${1:number})' },
  pow: { signature: 'pow(base, exponent)', description: 'Exponentiation.', snippet: 'pow(${1:base}, ${2:exponent})' },
  random: { signature: 'random([max] | [min, max])', description: 'Random number.', snippet: 'random(${1:10})' },
  random_seed: { signature: 'random_seed(seed)', description: 'Seeds the runtime RNG for reproducible results.', snippet: 'random_seed(${1:1234})' },
  random_int: { signature: 'random_int(max) | random_int(min, max)', description: 'Random integer.', snippet: 'random_int(${1:1}, ${2:100})' },
  random_float: { signature: 'random_float([max]) | random_float(min, max)', description: 'Random floating-point number.', snippet: 'random_float(${1:0}, ${2:1})' },
  random_bool: { signature: 'random_bool([probability])', description: 'Random boolean.', snippet: 'random_bool(${1:0.5})' },
  random_choice: { signature: 'random_choice(array)', description: 'Random item from a non-empty array.', snippet: 'random_choice(${1:items})' },
  random_shuffle: { signature: 'random_shuffle(array)', description: 'Shuffled shallow copy of an array.', snippet: 'random_shuffle(${1:items})' },
  random_bytes: { signature: 'random_bytes(count)', description: 'Array of random byte values.', snippet: 'random_bytes(${1:16})' },
  clamp: { signature: 'clamp(value, min, max)', description: 'Constrains a number.', snippet: 'clamp(${1:value}, ${2:min}, ${3:max})' },
  min: { signature: 'min(value, ...)', description: 'Smallest numeric argument.', snippet: 'min(${1:a}, ${2:b})' },
  max: { signature: 'max(value, ...)', description: 'Largest numeric argument.', snippet: 'max(${1:a}, ${2:b})' },

  split: { signature: 'split(text, separator)', description: 'Splits a string into an array.', snippet: 'split(${1:text}, ${2:","})' },
  join: { signature: 'join(array, separator)', description: 'Joins array values into a string.', snippet: 'join(${1:array}, ${2:","})' },
  trim: { signature: 'trim(text)', description: 'Trims whitespace.', snippet: 'trim(${1:text})' },
  upper: { signature: 'upper(text)', description: 'Uppercase string.', snippet: 'upper(${1:text})' },
  lower: { signature: 'lower(text)', description: 'Lowercase string.', snippet: 'lower(${1:text})' },
  contains: { signature: 'contains(text, needle)', description: 'Substring or array membership check.', snippet: 'contains(${1:text}, ${2:needle})' },
  startsWith: { signature: 'startsWith(text, prefix)', description: 'Prefix check.', snippet: 'startsWith(${1:text}, ${2:prefix})' },
  endsWith: { signature: 'endsWith(text, suffix)', description: 'Suffix check.', snippet: 'endsWith(${1:text}, ${2:suffix})' },
  indexOf: { signature: 'indexOf(value, needle)', description: 'Finds a string or array index.', snippet: 'indexOf(${1:value}, ${2:needle})' },
  substring: { signature: 'substring(text, start, [end])', description: 'String slice.', snippet: 'substring(${1:text}, ${2:start}, ${3:end})' },
  replace: { signature: 'replace(text, from, to)', description: 'Replaces all occurrences.', snippet: 'replace(${1:text}, ${2:from}, ${3:to})' },
  string_lines: { signature: 'string_lines(text)', description: 'Splits text into normalized lines.', snippet: 'string_lines(${1:text})' },
  string_words: { signature: 'string_words(text)', description: 'Splits text on whitespace into words.', snippet: 'string_words(${1:text})' },
  string_repeat: { signature: 'string_repeat(text, count)', description: 'Repeats text count times.', snippet: 'string_repeat(${1:text}, ${2:count})' },
  string_pad_left: { signature: 'string_pad_left(text, width, [fill])', description: 'Pads text on the left.', snippet: 'string_pad_left(${1:text}, ${2:width}, ${3:" "})' },
  string_pad_right: { signature: 'string_pad_right(text, width, [fill])', description: 'Pads text on the right.', snippet: 'string_pad_right(${1:text}, ${2:width}, ${3:" "})' },

  length: { signature: 'length(value)', description: 'Length of array, string, or dict.', snippet: 'length(${1:value})' },
  slice: { signature: 'slice(value, start, [end])', description: 'Array or string slice.', snippet: 'slice(${1:value}, ${2:start}, ${3:end})' },
  sort: { signature: 'sort(array)', description: 'Sorts an array in place.', snippet: 'sort(${1:array})' },
  reverse: { signature: 'reverse(value)', description: 'Reverses an array in place or returns reversed string.', snippet: 'reverse(${1:value})' },
  concat: { signature: 'concat(value, ...)', description: 'Concatenates arrays or strings.', snippet: 'concat(${1:a}, ${2:b})' },
  push: { signature: 'push(array, value, ...)', description: 'Appends values to an array.', snippet: 'push(${1:array}, ${2:value})' },
  pop: { signature: 'pop(array)', description: 'Removes and returns the last array value.', snippet: 'pop(${1:array})' },
  array_sum: { signature: 'array_sum(array)', description: 'Returns the sum of a numeric array.', snippet: 'array_sum(${1:array})' },
  array_avg: { signature: 'array_avg(array)', description: 'Returns the average of a numeric array.', snippet: 'array_avg(${1:array})' },
  array_unique: { signature: 'array_unique(array)', description: 'Returns a de-duplicated copy of an array.', snippet: 'array_unique(${1:array})' },
  array_flatten: { signature: 'array_flatten(array, [depth])', description: 'Returns an array flattened by depth levels.', snippet: 'array_flatten(${1:array}, ${2:1})' },
  array_range: { signature: 'array_range(end) | array_range(start, end, [step])', description: 'Returns numeric values excluding end.', snippet: 'array_range(${1:start}, ${2:end}, ${3:step})' },
  array_chunk: { signature: 'array_chunk(array, size)', description: 'Splits an array into fixed-size chunks.', snippet: 'array_chunk(${1:array}, ${2:size})' },
  array_zip: { signature: 'array_zip(array, ...)', description: 'Combines arrays into rows up to the shortest input length.', snippet: 'array_zip(${1:left}, ${2:right})' },
  array_repeat: { signature: 'array_repeat(value, count)', description: 'Returns an array with value repeated count times.', snippet: 'array_repeat(${1:value}, ${2:count})' },
  set_union: { signature: 'set_union(array, ...)', description: 'Returns the unique union of arrays.', snippet: 'set_union(${1:left}, ${2:right})' },
  set_intersection: { signature: 'set_intersection(array, ...)', description: 'Returns unique values present in every array.', snippet: 'set_intersection(${1:left}, ${2:right})' },
  set_difference: { signature: 'set_difference(array, ...)', description: 'Returns unique values from the first array that are missing from the rest.', snippet: 'set_difference(${1:left}, ${2:right})' },
  set_symmetric_difference: { signature: 'set_symmetric_difference(left, right)', description: 'Returns unique values present in exactly one of two arrays.', snippet: 'set_symmetric_difference(${1:left}, ${2:right})' },
  set_is_subset: { signature: 'set_is_subset(left, right)', description: 'Checks whether every value in left is present in right.', snippet: 'set_is_subset(${1:left}, ${2:right})' },
  set_is_superset: { signature: 'set_is_superset(left, right)', description: 'Checks whether every value in right is present in left.', snippet: 'set_is_superset(${1:left}, ${2:right})' },

  to_int: { signature: 'to_int(value)', description: 'Converts to integer.', snippet: 'to_int(${1:value})' },
  to_float: { signature: 'to_float(value)', description: 'Converts to floating point.', snippet: 'to_float(${1:value})' },
  to_str: { signature: 'to_str(value)', description: 'Converts to string.', snippet: 'to_str(${1:value})' },
  to_bool: { signature: 'to_bool(value)', description: 'Converts to boolean truthiness.', snippet: 'to_bool(${1:value})' },
  clone: { signature: 'clone(value)', description: 'Shallow copy for arrays, dicts, and instances.', snippet: 'clone(${1:value})' },
  Error: { signature: 'Error(message) | Error(type, message)', description: 'Creates an error dictionary.', snippet: 'Error(${1:"message"})' },
  assert: { signature: 'assert(condition, [message])', description: 'Fails when condition is falsey.', snippet: 'assert(${1:condition})' },
  assert_eq: { signature: 'assert_eq(actual, expected, [message])', description: 'Fails when two values are not equal.', snippet: 'assert_eq(${1:actual}, ${2:expected})' },
  assert_ne: { signature: 'assert_ne(actual, expected, [message])', description: 'Fails when two values are equal.', snippet: 'assert_ne(${1:actual}, ${2:expected})' },
  assert_neq: { signature: 'assert_neq(actual, expected, [message])', description: 'Alias for assert_ne.', snippet: 'assert_neq(${1:actual}, ${2:expected})' },
  check: { signature: 'check(name, condition, [message])', description: 'Returns a pass/fail test result dictionary without throwing.', snippet: 'check(${1:"name"}, ${2:condition})' },
  check_eq: { signature: 'check_eq(name, actual, expected, [message])', description: 'Returns a non-throwing equality test result dictionary.', snippet: 'check_eq(${1:"name"}, ${2:actual}, ${3:expected})' },
  check_match: { signature: 'check_match(name, text, pattern, [message])', description: 'Returns a non-throwing regex test result dictionary.', snippet: 'check_match(${1:"name"}, ${2:text}, ${3:"pattern"})' },
  test_summary: { signature: 'test_summary(results)', description: 'Summarizes check results with counts and failures.', snippet: 'test_summary(${1:results})' },
  test_report: { signature: 'test_report(results, [title])', description: 'Formats check results as a readable report.', snippet: 'test_report(${1:results}, ${2:"Sura tests"})' },

  file_read: { signature: 'file_read(path)', description: 'Reads a file as text.', snippet: 'file_read(${1:"path.txt"})' },
  file_write: { signature: 'file_write(path, text)', description: 'Writes text to a file.', snippet: 'file_write(${1:"path.txt"}, ${2:text})' },
  file_append: { signature: 'file_append(path, text)', description: 'Appends text to a file.', snippet: 'file_append(${1:"path.txt"}, ${2:text})' },
  file_exists: { signature: 'file_exists(path)', description: 'Checks whether a path exists.', snippet: 'file_exists(${1:"path.txt"})' },
  file_delete: { signature: 'file_delete(path)', description: 'Deletes a file.', snippet: 'file_delete(${1:"path.txt"})' },
  file_remove_tree: { signature: 'file_remove_tree(path)', description: 'Recursively removes a file or directory tree.', snippet: 'file_remove_tree(${1:"dir"})' },
  file_lines: { signature: 'file_lines(path)', description: 'Reads a file into an array of lines.', snippet: 'file_lines(${1:"path.txt"})' },
  file_list: { signature: 'file_list(path)', description: 'Lists directory entries.', snippet: 'file_list(${1:"."})' },
  file_walk: { signature: 'file_walk(path, [extension])', description: 'Recursively lists files, optionally filtering by extension.', snippet: 'file_walk(${1:"src"}, ${2:".sura"})' },
  file_is_dir: { signature: 'file_is_dir(path)', description: 'Checks whether a path is a directory.', snippet: 'file_is_dir(${1:"."})' },
  file_is_file: { signature: 'file_is_file(path)', description: 'Checks whether a path is a regular file.', snippet: 'file_is_file(${1:"path.txt"})' },
  file_size: { signature: 'file_size(path)', description: 'Returns the file size in bytes.', snippet: 'file_size(${1:"path.txt"})' },
  file_copy: { signature: 'file_copy(src, dst, [overwrite])', description: 'Copies a file.', snippet: 'file_copy(${1:"src.txt"}, ${2:"dst.txt"})' },
  file_move: { signature: 'file_move(src, dst, [overwrite])', description: 'Moves or renames a file.', snippet: 'file_move(${1:"src.txt"}, ${2:"dst.txt"})' },
  file_info: { signature: 'file_info(path)', description: 'Returns path metadata such as exists, size, type, and modified time.', snippet: 'file_info(${1:"path.txt"})' },
  mkdir: { signature: 'mkdir(path)', description: 'Creates directories.', snippet: 'mkdir(${1:"dir"})' },
  cwd: { signature: 'cwd()', description: 'Current working directory.', snippet: 'cwd()' },
  path_join: { signature: 'path_join(part, ...)', description: 'Joins path segments using the host platform separator.', snippet: 'path_join(${1:"dir"}, ${2:"file.txt"})' },
  path_basename: { signature: 'path_basename(path)', description: 'Returns the final path component.', snippet: 'path_basename(${1:path})' },
  path_dirname: { signature: 'path_dirname(path)', description: 'Returns the parent path.', snippet: 'path_dirname(${1:path})' },
  path_ext: { signature: 'path_ext(path)', description: 'Returns the file extension.', snippet: 'path_ext(${1:path})' },
  path_stem: { signature: 'path_stem(path)', description: 'Returns the final path component without its last extension.', snippet: 'path_stem(${1:path})' },
  path_normalize: { signature: 'path_normalize(path)', description: 'Returns a lexically normalized path.', snippet: 'path_normalize(${1:path})' },
  path_abs: { signature: 'path_abs(path)', description: 'Returns an absolute normalized path.', snippet: 'path_abs(${1:path})' },
  path_relative: { signature: 'path_relative(path, [base])', description: 'Returns path relative to base or the current directory.', snippet: 'path_relative(${1:path}, ${2:cwd()})' },
  json_parse: { signature: 'json_parse(text)', description: 'Parses JSON into Sura values.', snippet: 'json_parse(${1:text})' },
  json_try_parse: { signature: 'json_try_parse(text, [fallback])', description: 'Parses JSON or returns fallback/nil on invalid input.', snippet: 'json_try_parse(${1:text}, ${2:nil})' },
  json_stringify: { signature: 'json_stringify(value)', description: 'Serializes a Sura value to JSON.', snippet: 'json_stringify(${1:value})' },
  json_has_path: { signature: 'json_has_path(value, path)', description: 'Checks whether a nested JSON path exists.', snippet: 'json_has_path(${1:value}, ${2:"path"})' },
  json_merge_patch: { signature: 'json_merge_patch(target, patch)', description: 'Applies a JSON Merge Patch-style object update without mutating the target.', snippet: 'json_merge_patch(${1:target}, ${2:patch})' },
  json_delete_path: { signature: 'json_delete_path(value, path)', description: 'Removes a nested JSON path from a copied value.', snippet: 'json_delete_path(${1:value}, ${2:"path"})' },
  json_set_path: { signature: 'json_set_path(value, path, new_value)', description: 'Sets a nested JSON path on a copied value, creating containers as needed.', snippet: 'json_set_path(${1:value}, ${2:"path"}, ${3:new_value})' },
  dict_keys: { signature: 'dict_keys(dict)', description: 'Returns sorted dictionary keys.', snippet: 'dict_keys(${1:dict})' },
  dict_values: { signature: 'dict_values(dict)', description: 'Returns dictionary values in sorted key order.', snippet: 'dict_values(${1:dict})' },
  dict_items: { signature: 'dict_items(dict)', description: 'Returns sorted key/value dictionaries.', snippet: 'dict_items(${1:dict})' },
  dict_merge: { signature: 'dict_merge(dict, ...)', description: 'Returns a shallow merged dictionary.', snippet: 'dict_merge(${1:base}, ${2:patch})' },
  dict_pick: { signature: 'dict_pick(dict, keys)', description: 'Returns a dictionary with selected keys.', snippet: 'dict_pick(${1:dict}, ${2:keys})' },
  dict_omit: { signature: 'dict_omit(dict, keys)', description: 'Returns a dictionary without selected keys.', snippet: 'dict_omit(${1:dict}, ${2:keys})' },
  serialize: { signature: 'serialize(value)', description: 'Alias for json_stringify.', snippet: 'serialize(${1:value})' },
  deserialize: { signature: 'deserialize(text)', description: 'Alias for json_parse.', snippet: 'deserialize(${1:text})' },
  schema_validate: { signature: 'schema_validate(value, schema)', description: 'Checks a dictionary against a simple JSON-style schema.', snippet: 'schema_validate(${1:value}, ${2:schema})' },
  regex_match: { signature: 'regex_match(text, pattern)', description: 'Checks whether text matches a regular expression.', snippet: 'regex_match(${1:text}, ${2:pattern})' },
  regex_replace: { signature: 'regex_replace(text, pattern, replacement)', description: 'Replaces regular expression matches.', snippet: 'regex_replace(${1:text}, ${2:pattern}, ${3:replacement})' },
  regex_find_all: { signature: 'regex_find_all(text, pattern)', description: 'Returns all regular expression matches.', snippet: 'regex_find_all(${1:text}, ${2:pattern})' },
  regex_escape: { signature: 'regex_escape(text)', description: 'Escapes text so it can be used as a literal regex pattern.', snippet: 'regex_escape(${1:text})' },
  regex_capture: { signature: 'regex_capture(text, pattern)', description: 'Returns the first regex match plus capture groups, or nil.', snippet: 'regex_capture(${1:text}, ${2:pattern})' },
  regex_captures: { signature: 'regex_captures(text, pattern)', description: 'Returns all regex matches with capture groups.', snippet: 'regex_captures(${1:text}, ${2:pattern})' },
  regex_split: { signature: 'regex_split(text, pattern)', description: 'Splits text using a regular expression delimiter.', snippet: 'regex_split(${1:text}, ${2:pattern})' },
  datetime_now: { signature: 'datetime_now()', description: 'Current local datetime as text.', snippet: 'datetime_now()' },
  datetime_format: { signature: 'datetime_format(timestamp, format)', description: 'Formats a timestamp with strftime-style tokens.', snippet: 'datetime_format(${1:timestamp}, ${2:"%Y-%m-%d"})' },
  datetime_utc_format: { signature: 'datetime_utc_format(timestamp, format)', description: 'Formats a timestamp in UTC.', snippet: 'datetime_utc_format(${1:timestamp}, ${2:"%Y-%m-%dT%H:%M:%SZ"})' },
  datetime_parse: { signature: 'datetime_parse(text, [format])', description: 'Parses local datetime text into a Unix timestamp.', snippet: 'datetime_parse(${1:text}, ${2:"%Y-%m-%dT%H:%M:%S"})' },
  datetime_parts: { signature: 'datetime_parts(timestamp, [utc])', description: 'Returns date and time fields for a timestamp.', snippet: 'datetime_parts(${1:timestamp}, ${2:false})' },
  datetime_add: { signature: 'datetime_add(timestamp, seconds)', description: 'Adds seconds to a Unix timestamp.', snippet: 'datetime_add(${1:timestamp}, ${2:seconds})' },
  datetime_diff: { signature: 'datetime_diff(end_timestamp, start_timestamp)', description: 'Returns timestamp difference in seconds.', snippet: 'datetime_diff(${1:end_timestamp}, ${2:start_timestamp})' },
  timestamp: { signature: 'timestamp()', description: 'Current Unix timestamp.', snippet: 'timestamp()' },
  sha256: { signature: 'sha256(text)', description: 'SHA-256 digest for text.', snippet: 'sha256(${1:text})' },
  hmac_sha256: { signature: 'hmac_sha256(key, message)', description: 'Returns an HMAC-SHA256 signature for API authentication.', snippet: 'hmac_sha256(${1:key}, ${2:message})' },
  hex_encode: { signature: 'hex_encode(text)', description: 'Encodes text bytes as lowercase hexadecimal.', snippet: 'hex_encode(${1:text})' },
  hex_decode: { signature: 'hex_decode(hex)', description: 'Decodes lowercase hexadecimal text into bytes.', snippet: 'hex_decode(${1:hex})' },
  base64_encode: { signature: 'base64_encode(text)', description: 'Base64-encodes text.', snippet: 'base64_encode(${1:text})' },
  base64_decode: { signature: 'base64_decode(text)', description: 'Decodes Base64 text into bytes.', snippet: 'base64_decode(${1:text})' },
  base64_url_encode: { signature: 'base64_url_encode(text)', description: 'Base64url-encodes text without padding.', snippet: 'base64_url_encode(${1:text})' },
  base64_url_decode: { signature: 'base64_url_decode(text)', description: 'Decodes unpadded Base64url text.', snippet: 'base64_url_decode(${1:text})' },
  url_encode: { signature: 'url_encode(text)', description: 'Percent-encodes text for URLs.', snippet: 'url_encode(${1:text})' },
  url_decode: { signature: 'url_decode(text)', description: 'Decodes percent-encoded URL text.', snippet: 'url_decode(${1:text})' },
  url_parse: { signature: 'url_parse(url)', description: 'Parses a URL into fields and query params.', snippet: 'url_parse(${1:url})' },
  url_build: { signature: 'url_build(parts)', description: 'Builds a URL from fields or params.', snippet: 'url_build(${1:parts})' },
  query_build: { signature: 'query_build(params)', description: 'Builds a deterministic URL query string from a dictionary.', snippet: 'query_build(${1:params})' },
  query_parse: { signature: 'query_parse(query)', description: 'Parses a URL query string into a dictionary.', snippet: 'query_parse(${1:query})' },
  form_build: { signature: 'form_build(params)', description: 'Builds an application/x-www-form-urlencoded body from a dictionary.', snippet: 'form_build(${1:params})' },
  form_parse: { signature: 'form_parse(body)', description: 'Parses an application/x-www-form-urlencoded body into a dictionary.', snippet: 'form_parse(${1:body})' },
  auth_bearer: { signature: 'auth_bearer(token)', description: 'Builds an Authorization Bearer header dictionary.', snippet: 'auth_bearer(${1:token})' },
  auth_basic: { signature: 'auth_basic(username, password)', description: 'Builds an Authorization Basic header dictionary.', snippet: 'auth_basic(${1:username}, ${2:password})' },
  headers_merge: { signature: 'headers_merge(headers, ...)', description: 'Merges HTTP header dictionaries.', snippet: 'headers_merge(${1:headers}, ${2:{}})' },
  headers_get: { signature: 'headers_get(headers, name, [default])', description: 'Reads an HTTP header by name case-insensitively.', snippet: 'headers_get(${1:headers}, ${2:"content-type"}, ${3:nil})' },
  headers_has: { signature: 'headers_has(headers, name)', description: 'Returns true when an HTTP header exists case-insensitively.', snippet: 'headers_has(${1:headers}, ${2:"content-type"})' },
  headers_redact: { signature: 'headers_redact(headers, [names], [mask])', description: 'Returns a copy of HTTP headers with sensitive values masked for logs.', snippet: 'headers_redact(${1:headers}, ${2:[]}, ${3:"[REDACTED]"})' },
  cookie_parse: { signature: 'cookie_parse(header_or_headers)', description: 'Parses an HTTP Cookie header into a dictionary.', snippet: 'cookie_parse(${1:headers})' },
  cookie_build: { signature: 'cookie_build(cookies)', description: 'Builds an HTTP Cookie header value from a dictionary.', snippet: 'cookie_build(${1:cookies})' },
  cookie_get: { signature: 'cookie_get(header_or_cookies, name, [default])', description: 'Reads one cookie from headers or a cookie dictionary.', snippet: 'cookie_get(${1:headers}, ${2:"session"}, ${3:nil})' },
  http_content_type: { signature: 'http_content_type(headers_or_value, [default])', description: 'Extracts a normalized HTTP media type.', snippet: 'http_content_type(${1:headers}, ${2:""})' },
  http_charset: { signature: 'http_charset(headers_or_value, [default])', description: 'Extracts a charset from a Content-Type value.', snippet: 'http_charset(${1:headers}, ${2:""})' },
  http_is_json: { signature: 'http_is_json(headers_or_value)', description: 'Returns true for JSON HTTP media types.', snippet: 'http_is_json(${1:headers})' },
  http_status_ok: { signature: 'http_status_ok(status)', description: 'Returns true for 2xx HTTP status codes.', snippet: 'http_status_ok(${1:status})' },
  http_status_text: { signature: 'http_status_text(status)', description: 'Returns the HTTP reason phrase.', snippet: 'http_status_text(${1:status})' },
  http_status_retryable: { signature: 'http_status_retryable(status)', description: 'Returns true for transient HTTP status codes.', snippet: 'http_status_retryable(${1:status})' },
  http_retry_after: { signature: 'http_retry_after(headers_or_value, [default_ms])', description: 'Parses Retry-After into milliseconds.', snippet: 'http_retry_after(${1:headers}, ${2:0})' },
  http_backoff_delays: { signature: 'http_backoff_delays(attempts, [base_ms], [factor], [max_ms])', description: 'Builds capped exponential backoff delays.', snippet: 'http_backoff_delays(${1:attempts}, ${2:250}, ${3:2}, ${4:60000})' },
  cli_parse: { signature: 'cli_parse(text, [value_flags])', description: 'Parses shell-like flags and positional arguments.', snippet: 'cli_parse(${1:args}, ${2:[]})' },
  argv: { signature: 'argv()', description: 'Returns script command-line arguments passed after the script path.', snippet: 'argv()' },
  argc: { signature: 'argc()', description: 'Returns the number of script command-line arguments.', snippet: 'argc()' },
  script_name: { signature: 'script_name()', description: 'Returns the current script or loaded artifact path.', snippet: 'script_name()' },
  log_set_file: { signature: 'log_set_file(path, [append])', description: 'Sends log output to a UTF-8 file.', snippet: 'log_set_file(${1:"app.jsonl"}, ${2:false})' },
  log_set_json: { signature: 'log_set_json(enabled)', description: 'Switches log output between text and JSON Lines.', snippet: 'log_set_json(${1:true})' },
  log_set_level: { signature: 'log_set_level(level)', description: 'Sets the minimum emitted log level.', snippet: 'log_set_level(${1:"WARN"})' },
  log_get_level: { signature: 'log_get_level()', description: 'Returns the current minimum log level.', snippet: 'log_get_level()' },
  log_level: { signature: 'log_level([level])', description: 'Gets or sets the current minimum log level.', snippet: 'log_level(${1:"INFO"})' },
  log_event: { signature: 'log_event(level, message, [fields])', description: 'Writes a structured log event.', snippet: 'log_event(${1:"info"}, ${2:message}, ${3:{}})' },
  log_debug: { signature: 'log_debug(message)', description: 'Writes a debug log line.', snippet: 'log_debug(${1:message})' },
  log_info: { signature: 'log_info(message)', description: 'Writes an info log line.', snippet: 'log_info(${1:message})' },
  log_warn: { signature: 'log_warn(message)', description: 'Writes a warning log line.', snippet: 'log_warn(${1:message})' },
  log_error: { signature: 'log_error(message)', description: 'Writes an error log line.', snippet: 'log_error(${1:message})' },
  console_log: { signature: 'console_log(value, ...)', description: 'Writes console values to stdout.', snippet: 'console_log(${1:value})' },
  console_print: { signature: 'console_print(value, ...)', description: 'Alias for console_log.', snippet: 'console_print(${1:value})' },
  console_write: { signature: 'console_write(value, ...)', description: 'Writes console values without a newline.', snippet: 'console_write(${1:value})' },
  console_write_line: { signature: 'console_write_line(value, ...)', description: 'Writes console values with a newline.', snippet: 'console_write_line(${1:value})' },
  console_writeln: { signature: 'console_writeln(value, ...)', description: 'Alias for console_write_line.', snippet: 'console_writeln(${1:value})' },
  console_println: { signature: 'console_println(value, ...)', description: 'Alias for console_write_line.', snippet: 'console_println(${1:value})' },
  console_line: { signature: 'console_line([value, ...])', description: 'Writes one console line.', snippet: 'console_line(${1:value})' },
  console_info: { signature: 'console_info(value, ...)', description: 'Writes informational console values to stdout.', snippet: 'console_info(${1:value})' },
  console_debug: { signature: 'console_debug(value, ...)', description: 'Writes debug console values to stdout.', snippet: 'console_debug(${1:value})' },
  console_warn: { signature: 'console_warn(value, ...)', description: 'Writes warning console values to stderr.', snippet: 'console_warn(${1:value})' },
  console_error: { signature: 'console_error(value, ...)', description: 'Writes error console values to stderr.', snippet: 'console_error(${1:value})' },
  console_exception: { signature: 'console_exception(value, ...)', description: 'Alias for console_error.', snippet: 'console_exception(${1:value})' },
  console_raw: { signature: 'console_raw(value, ...)', description: 'Writes values without separators, indentation, or newline.', snippet: 'console_raw(${1:value})' },
  console_flush: { signature: 'console_flush()', description: 'Flushes stdout and stderr.', snippet: 'console_flush()' },
  console_json: { signature: 'console_json(value, [indent])', description: 'Prints a value as JSON or pretty JSON.', snippet: 'console_json(${1:value}, ${2:2})' },
  console_inspect: { signature: 'console_inspect(value, [indent])', description: 'Returns an inspectable string for a value.', snippet: 'console_inspect(${1:value}, ${2:2})' },
  console_hrtime: { signature: 'console_hrtime()', description: 'Returns a high-resolution monotonic timestamp in milliseconds.', snippet: 'console_hrtime()' },
  console_beep: { signature: 'console_beep()', description: 'Writes a terminal bell character.', snippet: 'console_beep()' },
  console_clear: { signature: 'console_clear()', description: 'Clears an interactive terminal when supported.', snippet: 'console_clear()' },
  console_assert: { signature: 'console_assert(condition, [message...])', description: 'Writes an assertion failure to stderr when condition is false.', snippet: 'console_assert(${1:condition}, ${2:message})' },
  console_time: { signature: 'console_time([label])', description: 'Starts or resets a named console timer.', snippet: 'console_time(${1:"default"})' },
  console_time_log: { signature: 'console_time_log([label], [message...])', description: 'Prints elapsed milliseconds without stopping the named timer.', snippet: 'console_time_log(${1:"default"}, ${2:message})' },
  console_time_end: { signature: 'console_time_end([label])', description: 'Stops a named console timer and returns elapsed milliseconds.', snippet: 'console_time_end(${1:"default"})' },
  console_time_stamp: { signature: 'console_time_stamp([label])', description: 'Writes a timestamp marker and returns milliseconds.', snippet: 'console_time_stamp(${1:"mark"})' },
  console_count: { signature: 'console_count([label])', description: 'Increments and prints a named counter.', snippet: 'console_count(${1:"default"})' },
  console_count_reset: { signature: 'console_count_reset([label])', description: 'Resets a named console counter.', snippet: 'console_count_reset(${1:"default"})' },
  console_table: { signature: 'console_table(value)', description: 'Prints arrays and dictionaries as a compact text table.', snippet: 'console_table(${1:value})' },
  console_dir: { signature: 'console_dir(value)', description: 'Prints a structured value using Sura runtime formatting.', snippet: 'console_dir(${1:value})' },
  console_dirxml: { signature: 'console_dirxml(value, ...)', description: 'Prints XML/structured values using Sura runtime formatting.', snippet: 'console_dirxml(${1:value})' },
  console_trace: { signature: 'console_trace([message...])', description: 'Writes a trace line with the current runtime line.', snippet: 'console_trace(${1:message})' },
  console_group: { signature: 'console_group([label...])', description: 'Starts an indented console output group.', snippet: 'console_group(${1:"group"})' },
  console_group_collapsed: { signature: 'console_group_collapsed([label...])', description: 'Alias for console_group in terminal output.', snippet: 'console_group_collapsed(${1:"group"})' },
  console_group_end: { signature: 'console_group_end()', description: 'Ends the current indented console output group.', snippet: 'console_group_end()' },
  console_profile: { signature: 'console_profile([label])', description: 'Starts a lightweight named console profile timer.', snippet: 'console_profile(${1:"default"})' },
  console_profile_end: { signature: 'console_profile_end([label])', description: 'Stops a console profile timer and returns elapsed milliseconds.', snippet: 'console_profile_end(${1:"default"})' },
  console_style: { signature: 'console_style(text, style_or_styles)', description: 'Returns text wrapped in ANSI style escape codes.', snippet: 'console_style(${1:text}, ${2:["bold", "green"]})' },
  console_color: { signature: 'console_color(text, fg, [bg])', description: 'Returns text wrapped in ANSI foreground/background color escape codes.', snippet: 'console_color(${1:text}, ${2:"green"})' },
  console_strip_ansi: { signature: 'console_strip_ansi(text)', description: 'Removes ANSI escape codes from text.', snippet: 'console_strip_ansi(${1:text})' },
  console_set_color: { signature: 'console_set_color(fg, [bg])', description: 'Sets the current terminal ANSI color.', snippet: 'console_set_color(${1:"green"})' },
  console_reset_color: { signature: 'console_reset_color()', description: 'Resets terminal ANSI color and style.', snippet: 'console_reset_color()' },
  console_is_tty: { signature: 'console_is_tty()', description: 'Returns whether stdout is an interactive terminal.', snippet: 'console_is_tty()' },
  console_width: { signature: 'console_width()', description: 'Returns terminal width in columns, or 0 when unknown.', snippet: 'console_width()' },
  console_height: { signature: 'console_height()', description: 'Returns terminal height in rows, or 0 when unknown.', snippet: 'console_height()' },
  console_size: { signature: 'console_size()', description: 'Returns terminal width, height, and is_tty fields.', snippet: 'console_size()' },
  console_status: { signature: 'console_status()', description: 'Returns terminal size and active console state counts.', snippet: 'console_status()' },
  console_input: { signature: 'console_input([prompt])', description: 'Reads one line from stdin.', snippet: 'console_input(${1:"prompt: "})' },
  console_read_line: { signature: 'console_read_line([prompt])', description: 'Reads one line from stdin.', snippet: 'console_read_line(${1:"prompt: "})' },
  console_readline: { signature: 'console_readline([prompt])', description: 'Alias for console_read_line.', snippet: 'console_readline(${1:"prompt: "})' },
  console_readLine: { signature: 'console_readLine([prompt])', description: 'Alias for console_read_line.', snippet: 'console_readLine(${1:"prompt: "})' },
  console_prompt: { signature: 'console_prompt([prompt])', description: 'Alias for console_read_line.', snippet: 'console_prompt(${1:"prompt: "})' },
  db_set: { signature: 'db_set(path, key, value)', description: 'Sets a value in a small JSON-backed key-value store.', snippet: 'db_set(${1:"data.json"}, ${2:key}, ${3:value})' },
  db_get: { signature: 'db_get(path, key, [default])', description: 'Reads a value from a JSON-backed key-value store.', snippet: 'db_get(${1:"data.json"}, ${2:key})' },
  db_has: { signature: 'db_has(path, key)', description: 'Checks whether a JSON-backed key-value store has a key.', snippet: 'db_has(${1:"data.json"}, ${2:key})' },
  db_delete: { signature: 'db_delete(path, key)', description: 'Deletes a key from a JSON-backed key-value store.', snippet: 'db_delete(${1:"data.json"}, ${2:key})' },
  db_keys: { signature: 'db_keys(path)', description: 'Returns sorted keys from a JSON-backed key-value store.', snippet: 'db_keys(${1:"data.json"})' },
  db_all: { signature: 'db_all(path)', description: 'Returns all key/value data from a JSON-backed key-value store.', snippet: 'db_all(${1:"data.json"})' },
  db_insert: { signature: 'db_insert(path, row)', description: 'Appends a dictionary row to a JSON document table.', snippet: 'db_insert(${1:"rows.json"}, ${2:{}})' },
  db_find: { signature: 'db_find(path, criteria)', description: 'Finds rows in a JSON document table by exact criteria.', snippet: 'db_find(${1:"rows.json"}, ${2:{kind: "item"}})' },
  db_count: { signature: 'db_count(path, [criteria])', description: 'Counts rows in a JSON document table.', snippet: 'db_count(${1:"rows.json"}, ${2:{}})' },
  db_update: { signature: 'db_update(path, criteria, patch)', description: 'Updates matching rows in a JSON document table.', snippet: 'db_update(${1:"rows.json"}, ${2:{id: 1}}, ${3:{}})' },
  db_remove: { signature: 'db_remove(path, criteria)', description: 'Removes matching rows from a JSON document table.', snippet: 'db_remove(${1:"rows.json"}, ${2:{kind: "old"}})' },
  db_query: { signature: 'db_query(path, [criteria], [options])', description: 'Filters rows with optional sort, offset, and limit.', snippet: 'db_query(${1:"rows.json"}, ${2:{}}, ${3:{sort: "score", desc: true, limit: 10}})' },
  vec_add: { signature: 'vec_add(a, b)', description: 'Adds two numeric vectors.', snippet: 'vec_add(${1:a}, ${2:b})' },
  vec_dot: { signature: 'vec_dot(a, b)', description: 'Dot product for two numeric vectors.', snippet: 'vec_dot(${1:a}, ${2:b})' },
  vec_scale: { signature: 'vec_scale(vector, factor)', description: 'Scales a numeric vector.', snippet: 'vec_scale(${1:vector}, ${2:factor})' },
  vec_norm: { signature: 'vec_norm(vector)', description: 'Euclidean vector norm.', snippet: 'vec_norm(${1:vector})' },
  vec3: { signature: 'vec3(x, y, z)', description: 'Creates a 3D vector array.', snippet: 'vec3(${1:x}, ${2:y}, ${3:z})' },
  vec3_transform4: { signature: 'vec3_transform4(vector, matrix4)', description: 'Transforms a 3D vector by a row-major 4x4 matrix.', snippet: 'vec3_transform4(${1:vector}, ${2:matrix4})' },
  mat4_identity: { signature: 'mat4_identity()', description: 'Creates a row-major 4x4 identity matrix.', snippet: 'mat4_identity()' },
  mat4_translate: { signature: 'mat4_translate(x, y, z)', description: 'Creates a row-major 4x4 translation matrix.', snippet: 'mat4_translate(${1:x}, ${2:y}, ${3:z})' },
  mat4_scale: { signature: 'mat4_scale(x, y, z)', description: 'Creates a row-major 4x4 scale matrix.', snippet: 'mat4_scale(${1:x}, ${2:y}, ${3:z})' },
  mat4_rotate_y: { signature: 'mat4_rotate_y(radians)', description: 'Creates a row-major 4x4 Y-axis rotation matrix.', snippet: 'mat4_rotate_y(${1:radians})' },
  mat4_mul: { signature: 'mat4_mul(left, right)', description: 'Multiplies two row-major 4x4 matrices.', snippet: 'mat4_mul(${1:left}, ${2:right})' },
  mesh_cube: { signature: 'mesh_cube([size], [center])', description: 'Creates a cube mesh dictionary with vertices, faces, and edges.', snippet: 'mesh_cube(${1:1})' },
  mesh_transform4: { signature: 'mesh_transform4(mesh, matrix4)', description: 'Transforms all mesh vertices by a row-major 4x4 matrix.', snippet: 'mesh_transform4(${1:mesh}, ${2:matrix4})' },
  mesh_bounds: { signature: 'mesh_bounds(mesh)', description: 'Returns min, max, size, and center vectors for a mesh.', snippet: 'mesh_bounds(${1:mesh})' },
  mesh_face_normals: { signature: 'mesh_face_normals(mesh)', description: 'Computes normalized face normals for a mesh.', snippet: 'mesh_face_normals(${1:mesh})' },
  camera_project: { signature: 'camera_project(point, camera, [width], [height])', description: 'Projects a 3D point into viewport coordinates.', snippet: 'camera_project(${1:point}, ${2:camera}, ${3:800}, ${4:600})' },
  tensor_shape: { signature: 'tensor_shape(tensor)', description: 'Returns tensor dimensions for nested arrays.', snippet: 'tensor_shape(${1:tensor})' },
  tensor_zeros: { signature: 'tensor_zeros(shape)', description: 'Creates a zero-filled tensor from a shape array.', snippet: 'tensor_zeros(${1:[2, 2]})' },
  tensor_fill: { signature: 'tensor_fill(shape, value)', description: 'Creates a tensor filled with a numeric value.', snippet: 'tensor_fill(${1:[2, 2]}, ${2:0})' },
  tensor_add: { signature: 'tensor_add(a, b)', description: 'Elementwise tensor addition with scalar support.', snippet: 'tensor_add(${1:a}, ${2:b})' },
  tensor_mul: { signature: 'tensor_mul(a, b)', description: 'Elementwise tensor multiplication with scalar support.', snippet: 'tensor_mul(${1:a}, ${2:b})' },
  tensor_clip: { signature: 'tensor_clip(tensor, min, max)', description: 'Clamps numeric tensor leaves to an inclusive range.', snippet: 'tensor_clip(${1:tensor}, ${2:min}, ${3:max})' },
  tensor_flatten: { signature: 'tensor_flatten(tensor)', description: 'Flattens a numeric tensor into one array.', snippet: 'tensor_flatten(${1:tensor})' },
  tensor_sum: { signature: 'tensor_sum(tensor)', description: 'Returns the sum of all numeric tensor leaves.', snippet: 'tensor_sum(${1:tensor})' },
  tensor_mean: { signature: 'tensor_mean(tensor)', description: 'Returns the average of numeric tensor leaves.', snippet: 'tensor_mean(${1:tensor})' },
  tensor_variance: { signature: 'tensor_variance(tensor)', description: 'Returns the population variance of numeric tensor leaves.', snippet: 'tensor_variance(${1:tensor})' },
  tensor_std: { signature: 'tensor_std(tensor)', description: 'Returns the population standard deviation of numeric tensor leaves.', snippet: 'tensor_std(${1:tensor})' },
  tensor_min: { signature: 'tensor_min(tensor)', description: 'Returns the minimum numeric tensor leaf.', snippet: 'tensor_min(${1:tensor})' },
  tensor_max: { signature: 'tensor_max(tensor)', description: 'Returns the maximum numeric tensor leaf.', snippet: 'tensor_max(${1:tensor})' },
  tensor_argmin: { signature: 'tensor_argmin(tensor)', description: 'Returns the flattened index of the first minimum numeric tensor leaf.', snippet: 'tensor_argmin(${1:tensor})' },
  tensor_argmax: { signature: 'tensor_argmax(tensor)', description: 'Returns the flattened index of the first maximum numeric tensor leaf.', snippet: 'tensor_argmax(${1:tensor})' },
  tensor_zscore: { signature: 'tensor_zscore(tensor)', description: 'Returns z-score normalized numeric tensor leaves while preserving shape.', snippet: 'tensor_zscore(${1:tensor})' },
  tensor_softmax: { signature: 'tensor_softmax(tensor)', description: 'Returns stable softmax probabilities while preserving tensor shape.', snippet: 'tensor_softmax(${1:tensor})' },
  tensor_transpose: { signature: 'tensor_transpose(matrix)', description: 'Transposes a 2D numeric matrix.', snippet: 'tensor_transpose(${1:matrix})' },
  tensor_matmul: { signature: 'tensor_matmul(a, b)', description: 'Matrix multiplication for 2D numeric tensors.', snippet: 'tensor_matmul(${1:a}, ${2:b})' },
  nn_mlp: { signature: 'nn_mlp(layer_sizes, [options])', description: 'Creates a JSON-serializable dense neural network.', snippet: 'nn_mlp(${1:[2, 8, 1]}, ${2:{task: "binary"}})' },
  nn_forward: { signature: 'nn_forward(model, inputs)', description: 'Runs one sample or a batch through a native neural network.', snippet: 'nn_forward(${1:model}, ${2:inputs})' },
  nn_predict: { signature: 'nn_predict(model, inputs)', description: 'Returns probabilities or regression outputs.', snippet: 'nn_predict(${1:model}, ${2:inputs})' },
  nn_train: { signature: 'nn_train(model, inputs, targets, [options])', description: 'Trains a model with native backpropagation and Adam or SGD.', snippet: 'nn_train(${1:model}, ${2:inputs}, ${3:targets}, ${4:{optimizer: "adam"}})' },
  nn_classify: { signature: 'nn_classify(model, inputs, [threshold])', description: 'Returns binary, multilabel, or softmax classes.', snippet: 'nn_classify(${1:model}, ${2:inputs}, ${3:0.5})' },
  nn_evaluate: { signature: 'nn_evaluate(model, inputs, targets, [options])', description: 'Returns dataset loss and classification accuracy.', snippet: 'nn_evaluate(${1:model}, ${2:inputs}, ${3:targets})' },
  nn_summary: { signature: 'nn_summary(model)', description: 'Returns architecture, activations, and parameter count.', snippet: 'nn_summary(${1:model})' },
  nn_one_hot: { signature: 'nn_one_hot(labels, class_count)', description: 'Converts class indexes to one-hot target vectors.', snippet: 'nn_one_hot(${1:labels}, ${2:class_count})' },
  nn_fit_standardizer: { signature: 'nn_fit_standardizer(inputs)', description: 'Fits reusable per-feature mean and scale statistics.', snippet: 'nn_fit_standardizer(${1:inputs})' },
  nn_standardize: { signature: 'nn_standardize(inputs, standardizer)', description: 'Applies per-feature standardization to a sample or batch.', snippet: 'nn_standardize(${1:inputs}, ${2:standardizer})' },
  nn_split: { signature: 'nn_split(inputs, targets, [options])', description: 'Creates deterministic paired train/test partitions.', snippet: 'nn_split(${1:inputs}, ${2:targets}, ${3:{test_ratio: 0.2}})' },
  nn_save: { signature: 'nn_save(model, path)', description: 'Validates and saves a native model as JSON.', snippet: 'nn_save(${1:model}, ${2:"model.json"})' },
  nn_load: { signature: 'nn_load(path)', description: 'Loads and validates a native model JSON file.', snippet: 'nn_load(${1:"model.json"})' },
  autograd_tensor: { signature: 'autograd_tensor(data, [options])', description: 'Creates packed float64/32/16/bfloat16 tensor storage.', snippet: 'autograd_tensor(${1:data}, ${2:{dtype: "float32", requires_grad: false}})' },
  autograd_parameter: { signature: 'autograd_parameter(data, [dtype_or_options])', description: 'Creates a typed trainable leaf tensor.', snippet: 'autograd_parameter(${1:data}, ${2:{dtype: "float32"}})' },
  autograd_zeros: { signature: 'autograd_zeros(shape, [options])', description: 'Creates a typed contiguous tensor filled with zeros.', snippet: 'autograd_zeros(${1:[2, 2]}, ${2:{dtype: "float32"}})' },
  autograd_ones: { signature: 'autograd_ones(shape, [options])', description: 'Creates a typed contiguous tensor filled with ones.', snippet: 'autograd_ones(${1:[2, 2]}, ${2:{dtype: "float32"}})' },
  autograd_randn: { signature: 'autograd_randn(shape, [options])', description: 'Creates a deterministic normal tensor. Options include dtype.', snippet: 'autograd_randn(${1:[2, 2]}, ${2:{seed: 42, dtype: "float32", requires_grad: false}})' },
  autograd_data: { signature: 'autograd_data(tensor)', description: 'Returns tensor values as a scalar or nested Sura arrays.', snippet: 'autograd_data(${1:tensor})' },
  autograd_grad: { signature: 'autograd_grad(tensor)', description: 'Returns the accumulated leaf gradient as nested arrays, or nil before a gradient exists.', snippet: 'autograd_grad(${1:tensor})' },
  autograd_grad_info: { signature: 'autograd_grad_info(tensor)', description: 'Returns gradient storage, loss-scale, leaf/requires-grad, and basic optimizer-readiness metadata without copying gradient values to the host.', snippet: 'autograd_grad_info(${1:tensor})' },
  autograd_dtype: { signature: 'autograd_dtype(tensor)', description: 'Returns the packed tensor storage dtype.', snippet: 'autograd_dtype(${1:tensor})' },
  autograd_device: { signature: 'autograd_device(tensor)', description: 'Returns cpu or the resident cuda:N device for a tensor.', snippet: 'autograd_device(${1:tensor})' },
  autograd_to: { signature: 'autograd_to(tensor, device)', description: 'Explicitly copies a tensor between CPU and CUDA devices.', snippet: 'autograd_to(${1:tensor}, ${2:"cuda"})' },
  autograd_storage_bytes: { signature: 'autograd_storage_bytes(tensor)', description: 'Returns packed data storage bytes.', snippet: 'autograd_storage_bytes(${1:tensor})' },
  autograd_cast: { signature: 'autograd_cast(tensor, dtype)', description: 'Differentiably casts tensor storage dtype.', snippet: 'autograd_cast(${1:tensor}, ${2:"float32"})' },
  autograd_shape: { signature: 'autograd_shape(tensor)', description: 'Returns the tensor shape as an array of dimensions.', snippet: 'autograd_shape(${1:tensor})' },
  autograd_numel: { signature: 'autograd_numel(tensor)', description: 'Returns the number of scalar values in a tensor.', snippet: 'autograd_numel(${1:tensor})' },
  autograd_limits: { signature: 'autograd_limits()', description: 'Returns active CPU/dtype, rank, element, graph, and memory limits plus tracked memory usage.', snippet: 'autograd_limits()' },
  autograd_autocast: { signature: 'autograd_autocast([state])', description: 'Queries or changes resident-CUDA matmul autocast. A setter returns the previous {enabled, dtype} state.', snippet: 'autograd_autocast(${1:{enabled: true, dtype: "bfloat16"}})' },
  autograd_item: { signature: 'autograd_item(tensor)', description: 'Extracts the number stored in a one-element tensor.', snippet: 'autograd_item(${1:tensor})' },
  autograd_detach: { signature: 'autograd_detach(tensor)', description: 'Copies tensor data into a new leaf tensor detached from the computation graph.', snippet: 'autograd_detach(${1:tensor})' },
  autograd_requires_grad: { signature: 'autograd_requires_grad(tensor)', description: 'Returns whether a tensor records operations for reverse-mode differentiation.', snippet: 'autograd_requires_grad(${1:tensor})' },
  autograd_set_requires_grad: { signature: 'autograd_set_requires_grad(tensor, requires_grad)', description: 'Enables or disables gradient tracking on a leaf tensor and returns that tensor.', snippet: 'autograd_set_requires_grad(${1:tensor}, ${2:true})' },
  autograd_add: { signature: 'autograd_add(left, right)', description: 'Adds tensors, numeric arrays, or scalars with trailing-dimension broadcasting.', snippet: 'autograd_add(${1:left}, ${2:right})' },
  autograd_sub: { signature: 'autograd_sub(left, right)', description: 'Subtracts tensors, numeric arrays, or scalars with trailing-dimension broadcasting.', snippet: 'autograd_sub(${1:left}, ${2:right})' },
  autograd_mul: { signature: 'autograd_mul(left, right)', description: 'Multiplies tensors, numeric arrays, or scalars with trailing-dimension broadcasting.', snippet: 'autograd_mul(${1:left}, ${2:right})' },
  autograd_div: { signature: 'autograd_div(left, right)', description: 'Divides tensors, numeric arrays, or scalars with trailing-dimension broadcasting.', snippet: 'autograd_div(${1:left}, ${2:right})' },
  autograd_neg: { signature: 'autograd_neg(tensor)', description: 'Negates every tensor value while preserving the gradient graph.', snippet: 'autograd_neg(${1:tensor})' },
  autograd_reshape: { signature: 'autograd_reshape(tensor, shape)', description: 'Reshapes a tensor while preserving its gradient graph. Shape may contain one inferred -1 dimension.', snippet: 'autograd_reshape(${1:tensor}, ${2:[-1, 1]})' },
  autograd_matmul: { signature: 'autograd_matmul(left, right, [options])', description: 'Multiplies tensors on CPU or resident CUDA, with optional float32, float16, or bfloat16 compute_dtype.', snippet: 'autograd_matmul(${1:left}, ${2:right}, ${3:{compute_dtype: "bfloat16"}})' },
  autograd_transpose: { signature: 'autograd_transpose(tensor, [axis1], [axis2])', description: 'Swaps the last two axes by default, or swaps two explicitly supplied axes.', snippet: 'autograd_transpose(${1:tensor}, ${2:-2}, ${3:-1})' },
  autograd_linear: { signature: 'autograd_linear(inputs, weights, [bias])', description: 'Applies differentiable rank-2-or-higher matrix multiplication and optional broadcast bias.', snippet: 'autograd_linear(${1:inputs}, ${2:weights}, ${3:bias})' },
  autograd_relu: { signature: 'autograd_relu(tensor)', description: 'Applies the differentiable ReLU activation elementwise.', snippet: 'autograd_relu(${1:tensor})' },
  autograd_tanh: { signature: 'autograd_tanh(tensor)', description: 'Applies the differentiable hyperbolic-tangent activation elementwise.', snippet: 'autograd_tanh(${1:tensor})' },
  autograd_sigmoid: { signature: 'autograd_sigmoid(tensor)', description: 'Applies a numerically stable differentiable sigmoid activation.', snippet: 'autograd_sigmoid(${1:tensor})' },
  autograd_gelu: { signature: 'autograd_gelu(tensor)', description: 'Applies exact differentiable GELU elementwise.', snippet: 'autograd_gelu(${1:tensor})' },
  autograd_layer_norm: { signature: 'autograd_layer_norm(tensor, [weight], [bias], [epsilon])', description: 'Normalizes the last dimension with optional affine weight and bias.', snippet: 'autograd_layer_norm(${1:tensor}, ${2:weight}, ${3:bias}, ${4:0.00001})' },
  autograd_embedding: { signature: 'autograd_embedding(token_ids, weight)', description: 'Looks up token IDs in a [vocabulary, dimensions] weight tensor with scatter-add gradients.', snippet: 'autograd_embedding(${1:token_ids}, ${2:weight})' },
  autograd_causal_attention: { signature: 'autograd_causal_attention(query, key, value, [options])', description: 'Applies scaled causal attention. precision auto selects optimized/fallback paths, fast requires fused CUDA, and strict pins the f64 reference.', snippet: 'autograd_causal_attention(${1:query}, ${2:key}, ${3:value}, ${4:{precision: "auto"}})' },
  autograd_softmax: { signature: 'autograd_softmax(tensor)', description: 'Applies stable differentiable softmax over the last tensor dimension.', snippet: 'autograd_softmax(${1:tensor})' },
  autograd_sum: { signature: 'autograd_sum(tensor)', description: 'Reduces all tensor values to a differentiable scalar sum.', snippet: 'autograd_sum(${1:tensor})' },
  autograd_mean: { signature: 'autograd_mean(tensor)', description: 'Reduces all tensor values to a differentiable scalar mean.', snippet: 'autograd_mean(${1:tensor})' },
  autograd_mse: { signature: 'autograd_mse(predictions, targets)', description: 'Returns mean squared error as a scalar tensor, with broadcast-compatible targets.', snippet: 'autograd_mse(${1:predictions}, ${2:targets})' },
  autograd_bce: { signature: 'autograd_bce(probabilities, targets)', description: 'Returns mean binary cross-entropy for probabilities and broadcast-compatible targets.', snippet: 'autograd_bce(${1:probabilities}, ${2:targets})' },
  autograd_bce_logits: { signature: 'autograd_bce_logits(logits, targets)', description: 'Returns numerically stable mean binary cross-entropy directly from logits.', snippet: 'autograd_bce_logits(${1:logits}, ${2:targets})' },
  autograd_cross_entropy: { signature: 'autograd_cross_entropy(logits, one_hot_targets)', description: 'Returns stable multiclass cross-entropy over the last dimension using one-hot targets.', snippet: 'autograd_cross_entropy(${1:logits}, ${2:one_hot_targets})' },
  autograd_cross_entropy_ids: { signature: 'autograd_cross_entropy_ids(logits, class_ids)', description: 'Returns stable multiclass cross-entropy over the last dimension using integer class IDs.', snippet: 'autograd_cross_entropy_ids(${1:logits}, ${2:class_ids})' },
  autograd_backward: { signature: 'autograd_backward(tensor, [gradient], [retain_graph])', description: 'Runs reverse-mode automatic differentiation. Non-scalar outputs require an explicit gradient.', snippet: 'autograd_backward(${1:loss})' },
  autograd_backward_scaled: { signature: 'autograd_backward_scaled(loss, scale, [retain_graph])', description: 'Runs resident-CUDA backward with a declared loss scale and records it on persistent leaf gradients.', snippet: 'autograd_backward_scaled(${1:loss}, ${2:65536})' },
  autograd_zero_grad: { signature: 'autograd_zero_grad(parameters)', description: 'Zeros gradients for one trainable tensor or a flat array of parameters.', snippet: 'autograd_zero_grad(${1:parameters})' },
  autograd_unscale_gradients: { signature: 'autograd_unscale_gradients(parameters, [scale])', description: 'Transactionally unscales resident-CUDA gradients and reports finite/found_inf without partial commit.', snippet: 'autograd_unscale_gradients(${1:parameters})' },
  autograd_sgd: { signature: 'autograd_sgd(parameters, learning_rate, [options])', description: 'Updates leaf parameters with SGD. Options: momentum and weight_decay.', snippet: 'autograd_sgd(${1:parameters}, ${2:0.01}, ${3:{momentum: 0, weight_decay: 0}})' },
  autograd_adam: { signature: 'autograd_adam(parameters, learning_rate, [options])', description: 'Updates leaf parameters with Adam. Options: beta1, beta2, epsilon, and weight_decay.', snippet: 'autograd_adam(${1:parameters}, ${2:0.001}, ${3:{beta1: 0.9, beta2: 0.999, epsilon: 0.00000001, weight_decay: 0}})' },
  autograd_reset_optimizer: { signature: 'autograd_reset_optimizer(parameters)', description: 'Clears Adam and SGD momentum state stored for the supplied parameters.', snippet: 'autograd_reset_optimizer(${1:parameters})' },
  autograd_grad_norm: { signature: 'autograd_grad_norm(parameters)', description: 'Returns the global L2 norm of gradients for one tensor or a parameter array.', snippet: 'autograd_grad_norm(${1:parameters})' },
  autograd_clip_grad_norm: { signature: 'autograd_clip_grad_norm(parameters, max_norm)', description: 'Clips gradients to a global L2 norm and returns the norm measured before clipping.', snippet: 'autograd_clip_grad_norm(${1:parameters}, ${2:1})' },
  autograd_save_checkpoint: { signature: 'autograd_save_checkpoint(state_dict, path, [options])', description: 'Writes a v3 SHA-256 checkpoint, including CUDA master and optimizer state unless optimizer is false.', snippet: 'autograd_save_checkpoint(${1:state_dict}, ${2:"model.surackpt"}, ${3:{optimizer: true}})' },
  autograd_load_checkpoint: { signature: 'autograd_load_checkpoint(path, [options])', description: 'Loads v1-v3 leaf tensors. Use device: "cuda" with optimizer: true for exact CUDA optimizer resume.', snippet: 'autograd_load_checkpoint(${1:"model.surackpt"}, ${2:{optimizer: true, device: "cuda"}})' },
  autograd_cuda_available: { signature: 'autograd_cuda_available()', description: 'Checks whether the NVIDIA Driver/PTX backend initialized.', snippet: 'autograd_cuda_available()' },
  autograd_cuda_info: { signature: 'autograd_cuda_info()', description: 'Returns CUDA device and kernel-coverage metadata.', snippet: 'autograd_cuda_info()' },
  autograd_cuda_stats: { signature: 'autograd_cuda_stats()', description: 'Returns CUDA allocation, transfer, and kernel-launch counters.', snippet: 'autograd_cuda_stats()' },
  autograd_cuda_reset_stats: { signature: 'autograd_cuda_reset_stats()', description: 'Resets CUDA counters while preserving current allocation accounting.', snippet: 'autograd_cuda_reset_stats()' },
  autograd_cuda_synchronize: { signature: 'autograd_cuda_synchronize()', description: 'Synchronizes the CUDA context.', snippet: 'autograd_cuda_synchronize()' },
  autograd_save_safetensors: { signature: 'autograd_save_safetensors(state_dict, path)', description: 'Writes typed PyTorch-compatible Safetensors weights.', snippet: 'autograd_save_safetensors(${1:state}, ${2:"model.safetensors"})' },
  autograd_load_safetensors: { signature: 'autograd_load_safetensors(path, [options])', description: 'Strictly loads typed Safetensors weights.', snippet: 'autograd_load_safetensors(${1:"model.safetensors"})' },
  autograd_save_onnx_weights: { signature: 'autograd_save_onnx_weights(state_dict, path)', description: 'Writes a valid ONNX model containing typed initializers.', snippet: 'autograd_save_onnx_weights(${1:state}, ${2:"model.onnx"})' },
  autograd_load_onnx_weights: { signature: 'autograd_load_onnx_weights(path, [options])', description: 'Loads supported raw-data ONNX initializers, not arbitrary graph execution.', snippet: 'autograd_load_onnx_weights(${1:"model.onnx"})' },
  autograd_run_onnx: { signature: 'autograd_run_onnx(path, inputs, [options])', description: 'Executes the validated CPU ONNX opset 7-18 operator subset.', snippet: 'autograd_run_onnx(${1:"model.onnx"}, ${2:{input: tensor}})' },
  autograd_all_reduce_gradients: { signature: 'autograd_all_reduce_gradients(parameters, options)', description: 'Sums or averages gradients across shared-filesystem ranks.', snippet: 'autograd_all_reduce_gradients(${1:parameters}, ${2:{rendezvous: "rendezvous", run_id: "run", step: 0, rank: 0, world_size: 1}})' },
  tokenizer_byte: { signature: 'tokenizer_byte([options])', description: 'Creates a deterministic UTF-8 byte tokenizer.', snippet: 'tokenizer_byte(${1:{bos_id: 256, eos_id: 257}})' },
  tokenizer_train_bpe: { signature: 'tokenizer_train_bpe(corpus, [options])', description: 'Deterministically trains a bounded byte-level BPE tokenizer.', snippet: 'tokenizer_train_bpe(${1:corpus}, ${2:{vocab_size: 384, min_frequency: 2}})' },
  tokenizer_encode: { signature: 'tokenizer_encode(tokenizer, text, [options])', description: 'Encodes UTF-8 text with a byte or byte-level BPE tokenizer.', snippet: 'tokenizer_encode(${1:tokenizer}, ${2:text})' },
  tokenizer_decode: { signature: 'tokenizer_decode(tokenizer, ids, [options])', description: 'Losslessly decodes byte or BPE token IDs to UTF-8 text.', snippet: 'tokenizer_decode(${1:tokenizer}, ${2:ids})' },
  tokenizer_info: { signature: 'tokenizer_info(tokenizer)', description: 'Returns tokenizer metadata.', snippet: 'tokenizer_info(${1:tokenizer})' },
  tokenizer_save: { signature: 'tokenizer_save(tokenizer, path)', description: 'Saves a versioned tokenizer file.', snippet: 'tokenizer_save(${1:tokenizer}, ${2:"model.suratok"})' },
  tokenizer_load: { signature: 'tokenizer_load(path)', description: 'Loads and validates a tokenizer file.', snippet: 'tokenizer_load(${1:"model.suratok"})' },
  dataset_pack_text: { signature: 'dataset_pack_text(source, tokenizer, path, [options])', description: 'Streams text or files into a packed uint32 token dataset.', snippet: 'dataset_pack_text(${1:source}, ${2:tokenizer}, ${3:"train.suradata"})' },
  dataset_open: { signature: 'dataset_open(path, [options])', description: 'Opens a deterministic bounded seek-based loader.', snippet: 'dataset_open(${1:"train.suradata"}, ${2:{batch_size: 8, sequence_length: 128}})' },
  dataset_next: { signature: 'dataset_next(loader)', description: 'Reads one shifted input/target token batch.', snippet: 'dataset_next(${1:loader})' },
  dataset_reset: { signature: 'dataset_reset(loader, [epoch])', description: 'Resets loader position and epoch shuffle.', snippet: 'dataset_reset(${1:loader}, ${2:0})' },
  dataset_close: { signature: 'dataset_close(loader)', description: 'Closes a logical dataset handle.', snippet: 'dataset_close(${1:loader})' },
  dataset_info: { signature: 'dataset_info(loader)', description: 'Returns dataset and loader metadata.', snippet: 'dataset_info(${1:loader})' },
  media_available: { signature: 'media_available([ffmpeg_path])', description: 'Checks whether the configured FFmpeg decoder is executable.', snippet: 'media_available()' },
  media_ffmpeg_available: { signature: 'media_ffmpeg_available([ffmpeg_path])', description: 'Alias for media_available.', snippet: 'media_ffmpeg_available()' },
  media_frame_to_text: { signature: 'media_frame_to_text(pixels, [options])', description: 'Converts a grayscale, RGB, or RGBA matrix to one ASCII text frame.', snippet: 'media_frame_to_text(${1:pixels})' },
  media_ascii_frames: { signature: 'media_ascii_frames(path, [options])', description: 'Safely decodes a local video into bounded ASCII text frames.', snippet: 'media_ascii_frames(${1:"video.mp4"}, ${2:{width: 80, fps: 8, max_frames: 300}})' },
  media_video_to_text: { signature: 'media_video_to_text(path, [options])', description: 'Alias for media_ascii_frames.', snippet: 'media_video_to_text(${1:"video.mp4"})' },
  media_video_text_frames: { signature: 'media_video_text_frames(path, [options])', description: 'Alias for media_ascii_frames.', snippet: 'media_video_text_frames(${1:"video.mp4"})' },
  stream_from: { signature: 'stream_from(array_or_text)', description: 'Creates a pull stream from an array or line-based text.', snippet: 'stream_from(${1:items})' },
  stream_next: { signature: 'stream_next(stream)', description: 'Returns the next stream item or nil.', snippet: 'stream_next(${1:stream})' },
  stream_take: { signature: 'stream_take(stream, count)', description: 'Consumes and returns up to count stream items.', snippet: 'stream_take(${1:stream}, ${2:10})' },
  stream_batch: { signature: 'stream_batch(stream, size)', description: 'Consumes remaining stream items in fixed-size batches.', snippet: 'stream_batch(${1:stream}, ${2:100})' },
  stream_map: { signature: 'stream_map(stream, path, [fallback])', description: 'Consumes remaining stream items and returns a stream of JSON-path values.', snippet: 'stream_map(${1:stream}, ${2:"field"})' },
  stream_filter: { signature: 'stream_filter(stream, criteria)', description: 'Consumes remaining stream items and returns a stream of matching dictionaries.', snippet: 'stream_filter(${1:stream}, ${2:{kind: "event"}})' },
  stream_window: { signature: 'stream_window(stream, size, [step])', description: 'Consumes remaining stream items and returns rolling windows.', snippet: 'stream_window(${1:stream}, ${2:3})' },
  stream_skip: { signature: 'stream_skip(stream, count)', description: 'Consumes up to count stream items and returns the skipped count.', snippet: 'stream_skip(${1:stream}, ${2:10})' },
  stream_count: { signature: 'stream_count(stream)', description: 'Returns the number of remaining stream items.', snippet: 'stream_count(${1:stream})' },
  stream_join: { signature: 'stream_join(stream, [separator])', description: 'Consumes remaining stream items and joins them as text.', snippet: 'stream_join(${1:stream}, ${2:","})' },
  stream_sum: { signature: 'stream_sum(stream, [path])', description: 'Consumes remaining numeric stream items and returns their sum.', snippet: 'stream_sum(${1:stream}, ${2:"field"})' },
  stream_avg: { signature: 'stream_avg(stream, [path])', description: 'Consumes remaining numeric stream items and returns their average.', snippet: 'stream_avg(${1:stream}, ${2:"field"})' },
  stream_collect: { signature: 'stream_collect(stream)', description: 'Collects all remaining stream items.', snippet: 'stream_collect(${1:stream})' },
  stream_lines: { signature: 'stream_lines(path)', description: 'Creates a line stream from a file.', snippet: 'stream_lines(${1:"path.txt"})' },
  llm_message: { signature: 'llm_message(role, content)', description: 'Builds an OpenAI-style chat message dictionary.', snippet: 'llm_message(${1:"user"}, ${2:content})' },
  llm_messages: { signature: 'llm_messages([system], user)', description: 'Builds a chat messages array.', snippet: 'llm_messages(${1:"You are helpful"}, ${2:"Hi"})' },
  llm_request: { signature: 'llm_request(model, messages, [temperature])', description: 'Builds an OpenAI-style chat request dictionary.', snippet: 'llm_request(${1:"model"}, ${2:messages}, ${3:0.2})' },
  llm_request_json: { signature: 'llm_request_json(model, messages, [temperature])', description: 'Serializes an LLM chat request.', snippet: 'llm_request_json(${1:"model"}, ${2:messages}, ${3:0.2})' },
  llm_extract_text: { signature: 'llm_extract_text(response)', description: 'Extracts assistant text from an OpenAI-style response.', snippet: 'llm_extract_text(${1:response})' },
  llm_usage: { signature: 'llm_usage(response)', description: 'Normalizes LLM token usage fields from OpenAI-style responses.', snippet: 'llm_usage(${1:response})' },
  llm_cost: { signature: 'llm_cost(response, pricing)', description: 'Calculates LLM input/output costs from normalized token usage and per-million pricing.', snippet: 'llm_cost(${1:response}, ${2:pricing})' },
  llm_budget: { signature: 'llm_budget(response, pricing, limit)', description: 'Calculates LLM cost and reports whether it is within a budget limit.', snippet: 'llm_budget(${1:response}, ${2:pricing}, ${3:limit})' },
  llm_chat: { signature: 'llm_chat(endpoint, api_key, model, messages, [temperature])', description: 'Calls an OpenAI-compatible chat endpoint and returns raw JSON text.', snippet: 'llm_chat(${1:endpoint}, ${2:api_key}, ${3:model}, ${4:messages})' },
  http_get: { signature: 'http_get(url)', description: 'Fetches http, https, or file URLs as text.', snippet: 'http_get(${1:"https://example.com"})' },
  http_json: { signature: 'http_json(url)', description: 'Fetches a URL and parses the response as JSON.', snippet: 'http_json(${1:"https://example.com/data.json"})' },
  http_post: { signature: 'http_post(url, body, [content_type])', description: 'Sends an HTTP POST request.', snippet: 'http_post(${1:"https://example.com"}, ${2:body})' },
  http_request: { signature: 'http_request(spec)', description: 'Sends an HTTP request spec with url, query, headers, body/json, and timeout.', snippet: 'http_request({url: ${1:"https://example.com"}, query: ${2:{}}})' },
  http_request_full: { signature: 'http_request_full(spec)', description: 'Sends an HTTP request spec and returns status, headers, and body.', snippet: 'http_request_full({url: ${1:"https://example.com"}})' },
  http_request_retry: { signature: 'http_request_retry(spec, [attempts], [delay_ms])', description: 'Retries an HTTP request until it returns an ok response.', snippet: 'http_request_retry({url: ${1:"https://example.com"}}, ${2:3}, ${3:250})' },
  http_request_json: { signature: 'http_request_json(spec)', description: 'Sends an HTTP request spec and parses JSON.', snippet: 'http_request_json({url: ${1:"https://example.com/data.json"}})' },
  http_request_json_checked: { signature: 'http_request_json_checked(spec)', description: 'Requires a 2xx HTTP response before parsing JSON.', snippet: 'http_request_json_checked({url: ${1:"https://example.com/data.json"}})' },
  http_request_retry_json: { signature: 'http_request_retry_json(spec, [attempts], [delay_ms])', description: 'Retries an HTTP request and parses the response body as JSON.', snippet: 'http_request_retry_json({url: ${1:"https://example.com/data.json"}}, ${2:3}, ${3:250})' },
  http_request_retry_json_checked: { signature: 'http_request_retry_json_checked(spec, [attempts], [delay_ms])', description: 'Retries an HTTP request and requires a 2xx final response before parsing JSON.', snippet: 'http_request_retry_json_checked({url: ${1:"https://example.com/data.json"}}, ${2:3}, ${3:250})' },
  async_http_get: { signature: 'async_http_get(url, [scope_id])', description: 'Fetches a URL on the bounded async runtime and returns a task id.', snippet: 'async_http_get(${1:"https://example.com"}, ${2:scope})' },
  async_http_request: { signature: 'async_http_request(spec, [scope_id])', description: 'Sends an HTTP request spec on the bounded async runtime and returns a task id.', snippet: 'async_http_request({url: ${1:"https://example.com"}, query: ${2:{}}}, ${3:scope})' },
  async_sleep: { signature: 'async_sleep(milliseconds, [scope_id])', description: 'Starts a cooperatively cancellable timer task.', snippet: 'async_sleep(${1:1000}, ${2:scope})' },
  http_serve_static: { signature: 'http_serve_static(path, [port])', description: 'Starts a static file server task.', snippet: 'http_serve_static(${1:"."}, ${2:8000})' },
  tool_call: { signature: 'tool_call(spec)', description: 'Runs a declared automation tool such as shell or http_get.', snippet: 'tool_call(${1:spec})' },
  tool: { signature: 'tool(spec) | tool <name> { ... }', description: 'Short alias for tool_call and contextual command syntax for automation tools.', snippet: 'tool ${1:http_get} {${2:url}: ${3:"https://example.com"}}' },
  tool_spec: { signature: 'tool_spec(name, args)', description: 'Builds and validates a typed tool spec dictionary.', snippet: 'tool_spec(${1:"http_get"}, {${2:url}: ${3:"https://example.com"}})' },
  tool_validate: { signature: 'tool_validate(spec)', description: 'Returns true when a raw tool spec has the required typed fields.', snippet: 'tool_validate(${1:spec})' },
  tool_schema: { signature: 'tool_schema(name)', description: 'Returns the required fields for a built-in tool.', snippet: 'tool_schema(${1:"http_get"})' },
  tool_allowed: { signature: 'tool_allowed(spec, policy)', description: 'Returns true when a tool spec is allowed by a policy dictionary.', snippet: 'tool_allowed(${1:spec}, ${2:policy})' },
  tool_call_policy: { signature: 'tool_call_policy(spec, policy)', description: 'Runs a tool only when the policy allows it.', snippet: 'tool_call_policy(${1:spec}, ${2:policy})' },
  tool_list: { signature: 'tool_list()', description: 'Returns the built-in tool names.', snippet: 'tool_list()' },
  python_available: { signature: 'python_available()', description: 'Returns true when a Python interpreter is available.', snippet: 'python_available()' },
  python_executable: { signature: 'python_executable()', description: 'Returns the Python executable selected by Sura.', snippet: 'python_executable()' },
  python_eval: { signature: 'python_eval(code)', description: 'Evaluates Python code and returns stdout.', snippet: 'python_eval(${1:code})' },
  python_call: { signature: 'python_call(module, function, [args], [kwargs])', description: 'Calls a Python function with JSON-compatible arguments.', snippet: 'python_call(${1:module}, ${2:function}, ${3:[]})' },
  python_call_json: { signature: 'python_call_json(module, function, [args], [kwargs])', description: 'Calls Python and parses JSON output into Sura values.', snippet: 'python_call_json(${1:module}, ${2:function}, ${3:[]})' },
  ffi_load: { signature: 'ffi_load(path)', description: 'Loads a native dynamic library path for simple C ABI calls.', snippet: 'ffi_load(${1:"native.dll"})' },
  ffi_call: { signature: 'ffi_call(lib, symbol, signature, ...args)', description: 'Calls a simple C ABI function with int or double arguments.', snippet: 'ffi_call(${1:lib}, ${2:"symbol"}, ${3:"int(int,int)"})' },
  plugin_load: { signature: 'plugin_load(path)', description: 'Loads a native Sura plugin library.', snippet: 'plugin_load(${1:"plugin.dll"})' },
  plugin_load_manifest: { signature: 'plugin_load_manifest(path)', description: 'Loads a native plugin through a manifest allow-list.', snippet: 'plugin_load_manifest(${1:"native/plugin.sura-plugin.json"})' },
  plugin_call: { signature: 'plugin_call(plugin, export, ...args)', description: 'Calls an export from a loaded native plugin.', snippet: 'plugin_call(${1:plugin}, ${2:"native_add"}, ${3:1}, ${4:2})' },
  plugin_info: { signature: 'plugin_info(plugin)', description: 'Returns native plugin descriptor information.', snippet: 'plugin_info(${1:plugin})' },
  plugin_unload: { signature: 'plugin_unload(plugin)', description: 'Unloads a native plugin handle.', snippet: 'plugin_unload(${1:plugin})' },
  async_cmd: { signature: 'async_cmd(command, [scope_id])', description: 'Runs a cancellable child process on the bounded async runtime and returns a task id.', snippet: 'async_cmd(${1:"echo ok"}, ${2:scope})' },
  async_sura: { signature: 'async_sura(spec, [scope_id])', description: 'Runs a snapshotted Sura program in an isolated child process and returns a task id.', snippet: 'async_sura(${1:{program: "worker.sura", input: {}, timeout_ms: 30000}}, ${2:scope})' },
  task: { signature: 'task(command)', description: 'Alias for async_cmd.', snippet: 'task(${1:"echo ok"})' },
  async_ready: { signature: 'async_ready(task_id)', description: 'Checks whether an async task has completed.', snippet: 'async_ready(${1:task})' },
  async_status: { signature: 'async_status(task_id)', description: 'Returns task state without consuming output.', snippet: 'async_status(${1:task})' },
  async_pending: { signature: 'async_pending()', description: 'Returns status dictionaries for tracked async tasks.', snippet: 'async_pending()' },
  async_forget: { signature: 'async_forget(task_id)', description: 'Drops a completed async task without reading output.', snippet: 'async_forget(${1:task})' },
  async_cleanup: { signature: 'async_cleanup()', description: 'Drops all completed async tasks.', snippet: 'async_cleanup()' },
  async_cancel: { signature: 'async_cancel(task_id)', description: 'Cooperatively cancels a queued or running task.', snippet: 'async_cancel(${1:task})' },
  async_cancelled: { signature: 'async_cancelled(task_id)', description: 'Checks whether a retained task was cancelled.', snippet: 'async_cancelled(${1:task})' },
  async_configure: { signature: 'async_configure(max_workers, max_queue)', description: 'Configures a quiescent bounded worker pool.', snippet: 'async_configure(${1:4}, ${2:1024})' },
  async_limits: { signature: 'async_limits()', description: 'Returns async worker, queue, tracked-task, and scope counts.', snippet: 'async_limits()' },
  async_scope_open: { signature: 'async_scope_open()', description: 'Creates a structured-concurrency scope.', snippet: 'async_scope_open()' },
  async_scope_attach: { signature: 'async_scope_attach(scope_id, task_id)', description: 'Attaches an unscoped task to an open scope.', snippet: 'async_scope_attach(${1:scope}, ${2:task})' },
  async_scope_cancel: { signature: 'async_scope_cancel(scope_id)', description: 'Requests cancellation for every child in a scope.', snippet: 'async_scope_cancel(${1:scope})' },
  async_scope_status: { signature: 'async_scope_status(scope_id)', description: 'Returns aggregate child states for a scope.', snippet: 'async_scope_status(${1:scope})' },
  async_scope_close: { signature: 'async_scope_close(scope_id, [milliseconds])', description: 'Cancels, joins, cleans, and closes a scope.', snippet: 'async_scope_close(${1:scope}, ${2:5000})' },
  async_scope_join: { signature: 'async_scope_join(scope_id, [milliseconds])', description: 'Joins, cleans, and closes a scope without first cancelling children.', snippet: 'async_scope_join(${1:scope}, ${2:5000})' },
  async_await: { signature: 'async_await(task_id)', description: 'Waits for an async task and returns captured output.', snippet: 'async_await(${1:task})' },
  async_await_timeout: { signature: 'async_await_timeout(task_id, milliseconds, [default])', description: 'Waits up to a timeout without consuming pending tasks.', snippet: 'async_await_timeout(${1:task}, ${2:5000}, ${3:nil})' },
  async_ready_all: { signature: 'async_ready_all(task_ids)', description: 'Checks whether all async tasks are ready.', snippet: 'async_ready_all(${1:tasks})' },
  async_any: { signature: 'async_any(task_ids, [milliseconds], [default])', description: 'Waits for the first completed task.', snippet: 'async_any(${1:tasks}, ${2:5000}, ${3:nil})' },
  async_all: { signature: 'async_all(task_ids)', description: 'Waits for multiple async tasks.', snippet: 'async_all(${1:tasks})' },
  async_all_timeout: { signature: 'async_all_timeout(task_ids, milliseconds, [default])', description: 'Waits up to a timeout for all tasks.', snippet: 'async_all_timeout(${1:tasks}, ${2:5000}, ${3:nil})' },
  await: { signature: 'await(task_id)', description: 'Alias for async_await.', snippet: 'await(${1:task})' },
  await_timeout: { signature: 'await_timeout(task_id, milliseconds, [default])', description: 'Alias for async_await_timeout.', snippet: 'await_timeout(${1:task}, ${2:5000}, ${3:nil})' },
  await_any: { signature: 'await_any(task_ids, [milliseconds], [default])', description: 'Alias for async_any.', snippet: 'await_any(${1:tasks}, ${2:5000}, ${3:nil})' },
  await_all: { signature: 'await_all(task_ids)', description: 'Alias for async_all.', snippet: 'await_all(${1:tasks})' },
  await_all_timeout: { signature: 'await_all_timeout(task_ids, milliseconds, [default])', description: 'Alias for async_all_timeout.', snippet: 'await_all_timeout(${1:tasks}, ${2:5000}, ${3:nil})' },
  sleep_ms: { signature: 'sleep_ms(milliseconds)', description: 'Sleeps for a duration.', snippet: 'sleep_ms(${1:100})' },
  wait: { signature: 'wait(milliseconds)', description: 'Alias for sleep_ms.', snippet: 'wait(${1:100})' }
};

function moduleMember(signature: string, description: string, snippet: string): BuiltinInfo {
  return { signature, description, snippet };
}

function prefixedModuleMembers(moduleName: string, prefix: string): Record<string, BuiltinInfo> {
  const members: Record<string, BuiltinInfo> = {};
  for (const [builtinName, info] of Object.entries(BUILTINS)) {
    if (!builtinName.startsWith(prefix)) continue;
    const memberName = builtinName.slice(prefix.length);
    members[memberName] = moduleMember(
      info.signature.startsWith(builtinName)
        ? `${moduleName}.${memberName}${info.signature.slice(builtinName.length)}`
        : info.signature,
      info.description,
      info.snippet.startsWith(builtinName)
        ? `${memberName}${info.snippet.slice(builtinName.length)}`
        : info.snippet
    );
  }
  return members;
}

const CONSOLE_MEMBERS: Record<string, BuiltinInfo> = {
  log: moduleMember('console.log(value, ...)', 'Writes console values to stdout.', 'log(${1:value})'),
  print: moduleMember('console.print(value, ...)', 'Alias for console.log.', 'print(${1:value})'),
  write: moduleMember('console.write(value, ...)', 'Writes console values without a newline.', 'write(${1:value})'),
  write_line: moduleMember('console.write_line(value, ...)', 'Writes console values with a newline.', 'write_line(${1:value})'),
  writeln: moduleMember('console.writeln(value, ...)', 'Alias for console.write_line.', 'writeln(${1:value})'),
  println: moduleMember('console.println(value, ...)', 'Alias for console.write_line.', 'println(${1:value})'),
  line: moduleMember('console.line([value, ...])', 'Writes one console line.', 'line(${1:value})'),
  info: moduleMember('console.info(value, ...)', 'Writes informational console values to stdout.', 'info(${1:value})'),
  debug: moduleMember('console.debug(value, ...)', 'Writes debug console values to stdout.', 'debug(${1:value})'),
  warn: moduleMember('console.warn(value, ...)', 'Writes warning console values to stderr.', 'warn(${1:value})'),
  warning: moduleMember('console.warning(value, ...)', 'Alias for console.warn.', 'warning(${1:value})'),
  error: moduleMember('console.error(value, ...)', 'Writes error console values to stderr.', 'error(${1:value})'),
  exception: moduleMember('console.exception(value, ...)', 'Alias for console.error.', 'exception(${1:value})'),
  raw: moduleMember('console.raw(value, ...)', 'Writes values without separators, indentation, or newline.', 'raw(${1:value})'),
  flush: moduleMember('console.flush()', 'Flushes stdout and stderr.', 'flush()'),
  json: moduleMember('console.json(value, [indent])', 'Prints a value as JSON or pretty JSON.', 'json(${1:value}, ${2:2})'),
  inspect: moduleMember('console.inspect(value, [indent])', 'Returns an inspectable string for a value.', 'inspect(${1:value}, ${2:2})'),
  hrtime: moduleMember('console.hrtime()', 'Returns a high-resolution monotonic timestamp in milliseconds.', 'hrtime()'),
  beep: moduleMember('console.beep()', 'Writes a terminal bell character.', 'beep()'),
  clear: moduleMember('console.clear()', 'Clears an interactive terminal when supported.', 'clear()'),
  assert: moduleMember('console.assert(condition, [message...])', 'Writes an assertion failure to stderr when condition is false.', 'assert(${1:condition}, ${2:message})'),
  time: moduleMember('console.time([label])', 'Starts or resets a named console timer.', 'time(${1:"default"})'),
  time_log: moduleMember('console.time_log([label], [message...])', 'Prints elapsed milliseconds without stopping the named timer.', 'time_log(${1:"default"}, ${2:message})'),
  timeLog: moduleMember('console.timeLog([label], [message...])', 'Alias for console.time_log.', 'timeLog(${1:"default"}, ${2:message})'),
  time_end: moduleMember('console.time_end([label])', 'Stops a named console timer and returns elapsed milliseconds.', 'time_end(${1:"default"})'),
  timeEnd: moduleMember('console.timeEnd([label])', 'Alias for console.time_end.', 'timeEnd(${1:"default"})'),
  time_stamp: moduleMember('console.time_stamp([label])', 'Writes a timestamp marker and returns milliseconds.', 'time_stamp(${1:"mark"})'),
  timeStamp: moduleMember('console.timeStamp([label])', 'Alias for console.time_stamp.', 'timeStamp(${1:"mark"})'),
  count: moduleMember('console.count([label])', 'Increments and prints a named counter.', 'count(${1:"default"})'),
  count_reset: moduleMember('console.count_reset([label])', 'Resets a named console counter.', 'count_reset(${1:"default"})'),
  countReset: moduleMember('console.countReset([label])', 'Alias for console.count_reset.', 'countReset(${1:"default"})'),
  table: moduleMember('console.table(value)', 'Prints arrays and dictionaries as a compact text table.', 'table(${1:value})'),
  dir: moduleMember('console.dir(value)', 'Prints a structured value using Sura runtime formatting.', 'dir(${1:value})'),
  dirxml: moduleMember('console.dirxml(value, ...)', 'Prints XML/structured values using Sura runtime formatting.', 'dirxml(${1:value})'),
  trace: moduleMember('console.trace([message...])', 'Writes a trace line with the current runtime line.', 'trace(${1:message})'),
  group: moduleMember('console.group([label...])', 'Starts an indented console output group.', 'group(${1:"group"})'),
  group_collapsed: moduleMember('console.group_collapsed([label...])', 'Alias for console.group in terminal output.', 'group_collapsed(${1:"group"})'),
  groupCollapsed: moduleMember('console.groupCollapsed([label...])', 'Alias for console.group_collapsed.', 'groupCollapsed(${1:"group"})'),
  group_end: moduleMember('console.group_end()', 'Ends the current indented console output group.', 'group_end()'),
  groupEnd: moduleMember('console.groupEnd()', 'Alias for console.group_end.', 'groupEnd()'),
  profile: moduleMember('console.profile([label])', 'Starts a lightweight named console profile timer.', 'profile(${1:"default"})'),
  profile_end: moduleMember('console.profile_end([label])', 'Stops a console profile timer and returns elapsed milliseconds.', 'profile_end(${1:"default"})'),
  profileEnd: moduleMember('console.profileEnd([label])', 'Alias for console.profile_end.', 'profileEnd(${1:"default"})'),
  style: moduleMember('console.style(text, style_or_styles)', 'Returns text wrapped in ANSI style escape codes.', 'style(${1:text}, ${2:["bold", "green"]})'),
  color: moduleMember('console.color(text, fg, [bg])', 'Returns text wrapped in ANSI foreground/background color escape codes.', 'color(${1:text}, ${2:"green"})'),
  colour: moduleMember('console.colour(text, fg, [bg])', 'Alias for console.color.', 'colour(${1:text}, ${2:"green"})'),
  strip_ansi: moduleMember('console.strip_ansi(text)', 'Removes ANSI escape codes from text.', 'strip_ansi(${1:text})'),
  stripAnsi: moduleMember('console.stripAnsi(text)', 'Alias for console.strip_ansi.', 'stripAnsi(${1:text})'),
  set_color: moduleMember('console.set_color(fg, [bg])', 'Sets the current terminal ANSI color.', 'set_color(${1:"green"})'),
  setColor: moduleMember('console.setColor(fg, [bg])', 'Alias for console.set_color.', 'setColor(${1:"green"})'),
  reset_color: moduleMember('console.reset_color()', 'Resets terminal ANSI color and style.', 'reset_color()'),
  resetColor: moduleMember('console.resetColor()', 'Alias for console.reset_color.', 'resetColor()'),
  is_tty: moduleMember('console.is_tty()', 'Returns whether stdout is an interactive terminal.', 'is_tty()'),
  isTTY: moduleMember('console.isTTY()', 'Alias for console.is_tty.', 'isTTY()'),
  width: moduleMember('console.width()', 'Returns terminal width in columns, or 0 when unknown.', 'width()'),
  height: moduleMember('console.height()', 'Returns terminal height in rows, or 0 when unknown.', 'height()'),
  size: moduleMember('console.size()', 'Returns terminal width, height, and is_tty fields.', 'size()'),
  status: moduleMember('console.status()', 'Returns terminal size and active console state counts.', 'status()'),
  input: moduleMember('console.input([prompt])', 'Reads one line from stdin.', 'input(${1:"prompt: "})'),
  read_line: moduleMember('console.read_line([prompt])', 'Reads one line from stdin.', 'read_line(${1:"prompt: "})'),
  readline: moduleMember('console.readline([prompt])', 'Alias for console.read_line.', 'readline(${1:"prompt: "})'),
  readLine: moduleMember('console.readLine([prompt])', 'Alias for console.read_line.', 'readLine(${1:"prompt: "})'),
  prompt: moduleMember('console.prompt([prompt])', 'Alias for console.read_line.', 'prompt(${1:"prompt: "})')
};

const NN_MEMBERS = prefixedModuleMembers('nn', 'nn_');
const AI_MEMBERS = prefixedModuleMembers('ai', 'nn_');
const AUTOGRAD_MEMBERS = prefixedModuleMembers('autograd', 'autograd_');
const TOKENIZER_MEMBERS = prefixedModuleMembers('tokenizer', 'tokenizer_');
const DATASET_MEMBERS = prefixedModuleMembers('dataset', 'dataset_');
const MEDIA_MEMBERS = prefixedModuleMembers('media', 'media_');
const ASYNC_MEMBERS: Record<string, BuiltinInfo> = {
  ...prefixedModuleMembers('async', 'async_'),
  scope: moduleMember('async.scope()', 'Creates a structured-concurrency scope.', 'scope()')
};

const MODULE_MEMBERS: Record<string, Record<string, BuiltinInfo>> = {
  console: CONSOLE_MEMBERS,
  nn: NN_MEMBERS,
  ai: AI_MEMBERS,
  autograd: AUTOGRAD_MEMBERS,
  tokenizer: TOKENIZER_MEMBERS,
  dataset: DATASET_MEMBERS,
  media: MEDIA_MEMBERS,
  async: ASYNC_MEMBERS
};

function terminal(): vscode.Terminal {
  const name = vscode.workspace.getConfiguration('sura').get<string>('terminalName') || 'Sura';
  return vscode.window.terminals.find((t) => t.name === name) || vscode.window.createTerminal(name);
}

function quotePowerShellArg(value: string): string {
  return `'${value.replace(/'/g, "''")}'`;
}

function quoteCmdArg(value: string): string {
  return `"${value.replace(/"/g, '""')}"`;
}

function quotePosixArg(value: string): string {
  return `'${value.replace(/'/g, "'\\''")}'`;
}

function terminalShellKind(): 'powershell' | 'cmd' | 'posix' {
  if (process.platform !== 'win32') return 'posix';

  const config = vscode.workspace.getConfiguration('terminal.integrated');
  const defaultProfile = config.get<string>('defaultProfile.windows') || '';
  const automationProfile = config.get<{ path?: string; source?: string }>('automationProfile.windows') || {};
  const shellName = `${defaultProfile} ${automationProfile.path || ''} ${automationProfile.source || ''}`.toLowerCase();
  return shellName.includes('cmd') ? 'cmd' : 'powershell';
}

function terminalCommand(args: string[]): string {
  const shellKind = terminalShellKind();
  if (shellKind === 'powershell') {
    return `& ${args.map(quotePowerShellArg).join(' ')}`;
  }
  if (shellKind === 'cmd') {
    return args.map(quoteCmdArg).join(' ');
  }
  return args.map(quotePosixArg).join(' ');
}

function fileWorkspaceFolder(file?: string): vscode.WorkspaceFolder | undefined {
  return (file ? vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file)) : undefined) ||
    vscode.workspace.workspaceFolders?.[0];
}

function localEnginePath(file?: string): string | undefined {
  const workspaceFolder = fileWorkspaceFolder(file);
  const roots = [
    workspaceFolder?.uri.fsPath,
    file ? path.dirname(file) : undefined
  ].filter((root): root is string => !!root);
  const seen = new Set<string>();

  for (const root of roots) {
    for (const name of ['SuraLanguage.exe', 'SuraEngine.exe', 'sura.exe']) {
      const candidate = path.join(root, name);
      const key = candidate.toLowerCase();
      if (seen.has(key)) continue;
      seen.add(key);
      if (fs.existsSync(candidate)) return candidate;
    }
  }

  return undefined;
}

function installedWindowsEnginePath(): string | undefined {
  if (process.platform !== 'win32') return undefined;
  const localAppData = process.env.LOCALAPPDATA;
  if (!localAppData) return undefined;

  const binDir = path.join(localAppData, 'Programs', 'Sura', 'bin');
  for (const name of ['SuraLanguage.exe', 'SuraEngine.exe', 'sura.exe']) {
    const candidate = path.join(binDir, name);
    if (fs.existsSync(candidate)) return candidate;
  }

  return undefined;
}

function resolveConfiguredEnginePath(file: string | undefined, configured: string): string {
  if (path.isAbsolute(configured)) return configured;

  const workspaceFolder = fileWorkspaceFolder(file);
  const roots = [
    workspaceFolder?.uri.fsPath,
    file ? path.dirname(file) : undefined
  ].filter((root): root is string => !!root);

  for (const root of roots) {
    const candidate = path.resolve(root, configured);
    if (fs.existsSync(candidate)) return candidate;
  }

  return configured;
}

function enginePath(file?: string): string {
  const configured = vscode.workspace.getConfiguration('sura').get<string>('enginePath') || 'sura';
  if (configured !== 'sura') return resolveConfiguredEnginePath(file, configured);
  return localEnginePath(file) || installedWindowsEnginePath() || configured;
}

function packageManagerPath(file?: string): string {
  const configured = vscode.workspace.getConfiguration('sura').get<string>('packageManagerPath') ||
    (process.platform === 'win32' ? 'surapkg.exe' : 'surapkg');
  if (path.isAbsolute(configured)) return configured;

  const workspaceFolder = fileWorkspaceFolder(file);
  const roots = [
    workspaceFolder?.uri.fsPath,
    file ? path.dirname(file) : undefined
  ].filter((root): root is string => !!root);
  for (const root of roots) {
    const candidate = path.resolve(root, configured);
    if (fs.existsSync(candidate)) return candidate;
  }

  if (process.platform === 'win32' && process.env.LOCALAPPDATA) {
    const installed = path.join(process.env.LOCALAPPDATA, 'Programs', 'Sura', 'bin', 'surapkg.exe');
    if (fs.existsSync(installed)) return installed;
  }
  return configured;
}

function findPackageRoot(start?: string): string | undefined {
  const workspace = fileWorkspaceFolder(start)?.uri.fsPath;
  let current = start ? (fs.statSync(start).isDirectory() ? start : path.dirname(start)) : workspace;
  if (!current) return undefined;

  while (true) {
    if (fs.existsSync(path.join(current, 'sura.pkg.json'))) return current;
    const parent = path.dirname(current);
    if (parent === current) return undefined;
    current = parent;
  }
}

function packageTerminal(name: string, cwd: string): vscode.Terminal {
  return vscode.window.createTerminal({ name, cwd });
}

async function createStarterProject(): Promise<void> {
  const selected = await vscode.window.showOpenDialog({
    canSelectFiles: false,
    canSelectFolders: true,
    canSelectMany: false,
    defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri,
    openLabel: 'Choose project parent folder',
    title: 'Create a Sura Starter Project'
  });
  if (!selected?.length) return;

  const projectName = await vscode.window.showInputBox({
    prompt: 'Project name',
    placeHolder: 'hello_sura',
    validateInput: (value) => {
      const trimmed = value.trim();
      if (!trimmed) return 'Enter a project name.';
      if (!/^[A-Za-z_][A-Za-z0-9_-]*$/.test(trimmed)) {
        return 'Use letters, numbers, underscores, or hyphens; start with a letter or underscore.';
      }
      if (fs.existsSync(path.join(selected[0].fsPath, trimmed.replace(/-/g, '_')))) {
        return 'A folder with that normalized project name already exists.';
      }
      return undefined;
    }
  });
  if (!projectName) return;

  const term = packageTerminal('Sura Starter', selected[0].fsPath);
  term.show();
  term.sendText(terminalCommand([packageManagerPath(), 'new', projectName.trim()]));
}

function runPackageCommand(command: 'run' | 'test'): void {
  const file = vscode.window.activeTextEditor?.document.fileName;
  const root = findPackageRoot(file);
  if (!root) {
    vscode.window.showWarningMessage('Open a folder containing sura.pkg.json first.');
    return;
  }
  const term = packageTerminal(command === 'run' ? 'Sura Package' : 'Sura Tests', root);
  term.show();
  term.sendText(terminalCommand([packageManagerPath(file), command]));
}

function configuredLanguageArgs(): string[] {
  const lang = vscode.workspace.getConfiguration('sura').get<string>('language') || 'auto';
  return lang === 'en' || lang === 'ko' ? ['--lang', lang] : [];
}

function isSuraDocument(document: vscode.TextDocument): boolean {
  return document.languageId === 'sura' || path.extname(document.fileName) === '.sura';
}

function activeSuraFile(): string | undefined {
  const editor = vscode.window.activeTextEditor;
  if (!editor) {
    vscode.window.showWarningMessage('Open a Sura file first.');
    return undefined;
  }
  if (!isSuraDocument(editor.document)) {
    vscode.window.showWarningMessage('The active editor is not a Sura file.');
    return undefined;
  }
  editor.document.save();
  return editor.document.fileName;
}

function runSura(args: string[]): void {
  const file = activeSuraFile();
  if (!file) return;
  const term = terminal();
  term.show();
  term.sendText(terminalCommand([enginePath(file), ...configuredLanguageArgs(), ...args, file]));
}

async function debugSura(): Promise<void> {
  const file = activeSuraFile();
  if (!file) return;
  const uri = vscode.Uri.file(file);
  const workspaceFolder = vscode.workspace.getWorkspaceFolder(uri);
  const cwd = workspaceFolder?.uri.fsPath || path.dirname(file);
  await vscode.debug.startDebugging(workspaceFolder, {
    type: 'sura',
    request: 'launch',
    name: 'Debug Sura File',
    program: file,
    enginePath: enginePath(file),
    cwd,
    stopOnEntry: false
  });
}

class SuraRunCodeLensProvider implements vscode.CodeLensProvider {
  provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
    const enabled = vscode.workspace.getConfiguration('sura').get<boolean>('showRunCodeLens');
    if (enabled === false || !isSuraDocument(document) || document.lineCount < 1) {
      return [];
    }

    const range = new vscode.Range(0, 0, 0, 0);
    return [
      new vscode.CodeLens(range, {
        title: '$(play) Run Sura File',
        command: 'sura.runFile'
      }),
      new vscode.CodeLens(range, {
        title: '$(debug-alt) Debug Sura File',
        command: 'sura.debugFile'
      })
    ];
  }
}

class SuraStatusBarActions {
  private readonly runItem: vscode.StatusBarItem;
  private readonly debugItem: vscode.StatusBarItem;

  constructor() {
    this.runItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    this.runItem.name = 'Sura Run';
    this.runItem.text = '$(play) Run Sura';
    this.runItem.tooltip = 'Run the active Sura file';
    this.runItem.command = 'sura.runFile';

    this.debugItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
    this.debugItem.name = 'Sura Debug';
    this.debugItem.text = '$(debug-alt) Debug';
    this.debugItem.tooltip = 'Debug the active Sura file';
    this.debugItem.command = 'sura.debugFile';
  }

  public register(context: vscode.ExtensionContext): void {
    context.subscriptions.push(
      this.runItem,
      this.debugItem,
      vscode.window.onDidChangeActiveTextEditor(() => this.update()),
      vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration('sura.showStatusBarActions')) {
          this.update();
        }
      })
    );
    this.update();
  }

  private update(): void {
    const enabled = vscode.workspace.getConfiguration('sura').get<boolean>('showStatusBarActions');
    const editor = vscode.window.activeTextEditor;
    const visible = enabled !== false && !!editor && isSuraDocument(editor.document);

    if (visible) {
      this.runItem.show();
      this.debugItem.show();
    } else {
      this.runItem.hide();
      this.debugItem.hide();
    }
  }
}

class SuraCompletionProvider implements vscode.CompletionItemProvider {
  async provideCompletionItems(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.CompletionItem[]> {
    const memberContext = currentModuleMemberContext(document, position);
    if (memberContext) {
      return moduleMemberCompletions(memberContext.moduleName, memberContext.memberPrefix);
    }

    const prefix = currentWordPrefix(document, position).toLowerCase();
    const items: vscode.CompletionItem[] = [];
    const existingLabels = new Set<string>();
    const assignmentKeyword = assignmentKeywordCompletion(document, position, prefix);
    if (assignmentKeyword) {
      items.push(assignmentKeyword);
      existingLabels.add('is');
    }

    for (const keyword of KEYWORDS) {
      if (existingLabels.has(keyword) || !matchesPrefix(keyword, prefix)) continue;
      const item = new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword);
      item.detail = 'Sura keyword';
      item.filterText = keyword;
      item.sortText = `0_${keyword}`;
      items.push(item);
      existingLabels.add(keyword);
    }

    for (const [name, info] of Object.entries(BUILTINS)) {
      if (!matchesPrefix(name, prefix)) continue;
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
      item.detail = info.signature;
      item.documentation = new vscode.MarkdownString(`\`\`\`sura\n${info.signature}\n\`\`\`\n${info.description}`);
      item.insertText = new vscode.SnippetString(info.snippet);
      item.filterText = name;
      item.sortText = `0_${name}`;
      items.push(item);
      existingLabels.add(name);
    }

    for (const name of MODULES) {
      if (!matchesPrefix(name, prefix)) continue;
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Module);
      item.detail = `stdlib module (use ${name})`;
      item.documentation = new vscode.MarkdownString(`\`\`\`sura\nuse ${name}\n\`\`\``);
      item.filterText = name;
      item.sortText = `1_${name}`;
      items.push(item);
      existingLabels.add(name);
    }

    for (const symbol of await collectProjectSymbols(document, prefix)) {
      if (existingLabels.has(symbol.name)) continue;
      const item = new vscode.CompletionItem(symbol.name, symbol.kind);
      item.detail = symbol.detail;
      item.insertText = symbol.snippet ? new vscode.SnippetString(symbol.snippet) : symbol.name;
      item.filterText = symbol.name;
      item.sortText = `2_${symbol.name}`;
      items.push(item);
      existingLabels.add(symbol.name);
    }

    return items;
  }
}

async function collectProjectSymbols(document: vscode.TextDocument, prefix: string): Promise<ProjectSymbolInfo[]> {
  if (prefix.length === 0) return [];

  const symbols = new Map<string, ProjectSymbolInfo>();
  const addSymbols = (text: string, sourceLabel: string): void => {
    for (const symbol of extractSuraSymbols(text, sourceLabel)) {
      if (!matchesPrefix(symbol.name, prefix) || symbols.has(symbol.name)) continue;
      symbols.set(symbol.name, symbol);
    }
  };

  addSymbols(document.getText(), 'current file');

  const currentPath = path.normalize(document.uri.fsPath);
  const files = await vscode.workspace.findFiles(
    '**/*.sura',
    '{**/.git/**,**/node_modules/**,**/sura_packages/**,**/dist/**,**/build/**}',
    200
  );

  for (const uri of files) {
    if (path.normalize(uri.fsPath) === currentPath) continue;
    try {
      const bytes = await vscode.workspace.fs.readFile(uri);
      addSymbols(Buffer.from(bytes).toString('utf8'), path.basename(uri.fsPath));
    } catch {
      // Ignore unreadable files so completion remains responsive.
    }
  }

  return [...symbols.values()];
}

function extractSuraSymbols(text: string, sourceLabel: string): ProjectSymbolInfo[] {
  const symbols: ProjectSymbolInfo[] = [];
  const identifier = `(${IDENTIFIER_PATTERN})`;
  const funcPattern = new RegExp(`^\\s*func\\s+${identifier}\\s*\\(([^)]*)\\)`, 'u');
  const classPattern = new RegExp(`^\\s*class\\s+${identifier}\\b`, 'u');
  const enumPattern = new RegExp(`^\\s*enum\\s+${identifier}\\b`, 'u');
  const assignPattern = new RegExp(`^\\s*${identifier}\\s+is\\b`, 'u');
  const usePattern = new RegExp(`^\\s*use\\s+${identifier}\\b`, 'u');

  for (const line of text.split(/\r?\n/)) {
    let match = line.match(funcPattern);
    if (match) {
      symbols.push({
        name: match[1],
        kind: vscode.CompletionItemKind.Function,
        detail: `Sura function (${sourceLabel})`,
        snippet: functionSnippet(match[1], match[2])
      });
      continue;
    }

    match = line.match(classPattern);
    if (match) {
      symbols.push({ name: match[1], kind: vscode.CompletionItemKind.Class, detail: `Sura class (${sourceLabel})` });
      continue;
    }

    match = line.match(enumPattern);
    if (match) {
      symbols.push({ name: match[1], kind: vscode.CompletionItemKind.Enum, detail: `Sura enum (${sourceLabel})` });
      continue;
    }

    match = line.match(usePattern);
    if (match) {
      symbols.push({ name: match[1], kind: vscode.CompletionItemKind.Module, detail: `Sura module import (${sourceLabel})` });
      continue;
    }

    match = line.match(assignPattern);
    if (match) {
      symbols.push({ name: match[1], kind: vscode.CompletionItemKind.Variable, detail: `Sura variable (${sourceLabel})` });
    }
  }

  return symbols;
}

function functionSnippet(name: string, paramsText: string): string {
  const params = paramsText
    .split(',')
    .map((param) => param.trim().split(/[:\s]/)[0])
    .filter(Boolean);
  if (params.length === 0) return `${name}()`;
  return `${name}(${params.map((param, index) => '${' + (index + 1) + ':' + param + '}').join(', ')})`;
}

function currentWordPrefix(document: vscode.TextDocument, position: vscode.Position): string {
  const text = document.lineAt(position).text.slice(0, position.character);
  return text.match(new RegExp(`${IDENTIFIER_PATTERN}$`, 'u'))?.[0] || '';
}

function assignmentKeywordCompletion(document: vscode.TextDocument, position: vscode.Position, prefix: string): vscode.CompletionItem | undefined {
  if (!matchesPrefix('is', prefix)) return undefined;
  const text = document.lineAt(position).text.slice(0, position.character);
  const beforePrefix = text.slice(0, text.length - prefix.length);
  const match = beforePrefix.match(new RegExp(`^\\s*(${IDENTIFIER_PATTERN})\\s+$`, 'u'));
  if (!match) return undefined;

  const firstToken = match[1].toLowerCase();
  if (KEYWORDS.includes(firstToken) || MODULES.includes(firstToken) || Object.prototype.hasOwnProperty.call(BUILTINS, firstToken)) {
    return undefined;
  }

  const range = new vscode.Range(new vscode.Position(position.line, position.character - prefix.length), position);
  const item = new vscode.CompletionItem('is', vscode.CompletionItemKind.Keyword);
  item.detail = 'Sura assignment keyword';
  item.documentation = new vscode.MarkdownString('```sura\nname is value\n```');
  item.insertText = new vscode.SnippetString('is ${1:value}');
  item.filterText = 'is';
  item.sortText = '!_is_assignment';
  item.range = range;
  item.preselect = true;
  return item;
}

function currentModuleMemberContext(document: vscode.TextDocument, position: vscode.Position): { moduleName: string; memberPrefix: string } | undefined {
  const text = document.lineAt(position).text.slice(0, position.character);
  const match = text.match(new RegExp(`(${IDENTIFIER_PATTERN})\\.(${IDENTIFIER_PATTERN})?$`, 'u'));
  if (!match) return undefined;
  const moduleName = match[1];
  if (!MODULE_MEMBERS[moduleName]) return undefined;
  return { moduleName, memberPrefix: (match[2] || '').toLowerCase() };
}

function moduleMemberCompletions(moduleName: string, prefix: string): vscode.CompletionItem[] {
  return Object.entries(MODULE_MEMBERS[moduleName] || {})
    .filter(([name]) => matchesPrefix(name, prefix))
    .map(([name, info]) => {
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Method);
      item.detail = info.signature;
      item.documentation = new vscode.MarkdownString(`\`\`\`sura\n${info.signature}\n\`\`\`\n${info.description}`);
      item.insertText = new vscode.SnippetString(info.snippet);
      item.filterText = name;
      item.sortText = `0_${name}`;
      return item;
    });
}

function moduleMemberAt(document: vscode.TextDocument, range: vscode.Range): BuiltinInfo | undefined {
  const line = document.lineAt(range.start.line).text;
  const before = line.slice(0, range.start.character);
  const match = before.match(new RegExp(`(${IDENTIFIER_PATTERN})\\.$`, 'u'));
  if (!match) return undefined;
  const memberName = document.getText(range);
  return MODULE_MEMBERS[match[1]]?.[memberName];
}

function callableContextAt(line: string): { info: BuiltinInfo; activeParameter: number } | undefined {
  const openDelimiters: Array<{ char: string; index: number }> = [];
  let inString = false;
  let escaped = false;

  for (let index = 0; index < line.length; index += 1) {
    const char = line[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (char === '\\') escaped = true;
      else if (char === '"') inString = false;
      continue;
    }
    if (char === '"') {
      inString = true;
      continue;
    }
    if (char === '(' || char === '[' || char === '{') {
      openDelimiters.push({ char, index });
      continue;
    }
    const expected = char === ')' ? '(' : char === ']' ? '[' : char === '}' ? '{' : undefined;
    if (expected && openDelimiters[openDelimiters.length - 1]?.char === expected) openDelimiters.pop();
  }

  const call = [...openDelimiters].reverse().find((delimiter) => delimiter.char === '(');
  if (!call) return undefined;
  const callable = line.slice(0, call.index).match(/(?:([A-Za-z_][A-Za-z0-9_]*)\.)?([A-Za-z_][A-Za-z0-9_]*)\s*$/);
  if (!callable) return undefined;
  const info = callable[1] ? MODULE_MEMBERS[callable[1]]?.[callable[2]] : BUILTINS[callable[2]];
  if (!info) return undefined;

  let activeParameter = 0;
  let parens = 0;
  let brackets = 0;
  let braces = 0;
  inString = false;
  escaped = false;
  for (const char of line.slice(call.index + 1)) {
    if (inString) {
      if (escaped) escaped = false;
      else if (char === '\\') escaped = true;
      else if (char === '"') inString = false;
      continue;
    }
    if (char === '"') inString = true;
    else if (char === '(') parens += 1;
    else if (char === ')') parens = Math.max(0, parens - 1);
    else if (char === '[') brackets += 1;
    else if (char === ']') brackets = Math.max(0, brackets - 1);
    else if (char === '{') braces += 1;
    else if (char === '}') braces = Math.max(0, braces - 1);
    else if (char === ',' && parens === 0 && brackets === 0 && braces === 0) activeParameter += 1;
  }
  return { info, activeParameter };
}

function matchesPrefix(value: string, prefix: string): boolean {
  return prefix.length === 0 || value.toLowerCase().startsWith(prefix);
}

class SuraHoverProvider implements vscode.HoverProvider {
  provideHover(document: vscode.TextDocument, position: vscode.Position): vscode.Hover | undefined {
    const range = document.getWordRangeAtPosition(position);
    if (!range) return undefined;
    const word = document.getText(range);
    const member = moduleMemberAt(document, range);
    if (member) {
      return new vscode.Hover(new vscode.MarkdownString(`\`\`\`sura\n${member.signature}\n\`\`\`\n${member.description}`), range);
    }
    const builtin = BUILTINS[word];
    if (builtin) {
      return new vscode.Hover(new vscode.MarkdownString(`\`\`\`sura\n${builtin.signature}\n\`\`\`\n${builtin.description}`), range);
    }
    if (KEYWORDS.includes(word)) {
      return new vscode.Hover(new vscode.MarkdownString(`Sura keyword: \`${word}\``), range);
    }
    if (MODULES.includes(word)) {
      return new vscode.Hover(new vscode.MarkdownString(`Sura standard library module: \`use ${word}\``), range);
    }
    return undefined;
  }
}

class SuraSignatureProvider implements vscode.SignatureHelpProvider {
  provideSignatureHelp(document: vscode.TextDocument, position: vscode.Position): vscode.SignatureHelp | undefined {
    const line = document.lineAt(position).text.slice(0, position.character);
    const context = callableContextAt(line);
    if (!context) return undefined;

    const help = new vscode.SignatureHelp();
    const signature = new vscode.SignatureInformation(context.info.signature, context.info.description);
    const params = context.info.signature.match(/\(([^)]*)\)/)?.[1];
    if (params) {
      for (const param of params.split(',').map((p) => p.trim()).filter(Boolean)) {
        signature.parameters.push(new vscode.ParameterInformation(param));
      }
    }
    help.signatures = [signature];
    help.activeSignature = 0;
    help.activeParameter = Math.min(context.activeParameter, Math.max(0, signature.parameters.length - 1));
    return help;
  }
}

let languageClient: LanguageClient | undefined;
let fallbackLanguageDisposables: vscode.Disposable[] = [];

function executableAvailable(command: string): boolean {
  if (path.isAbsolute(command) || command.includes('/') || command.includes('\\')) {
    return fs.existsSync(command);
  }

  const pathEntries = (process.env.PATH || '').split(path.delimiter).filter(Boolean);
  const extensions = process.platform === 'win32'
    ? (process.env.PATHEXT || '.EXE;.CMD;.BAT;.COM').split(';')
    : [''];
  for (const entry of pathEntries) {
    for (const extension of extensions) {
      if (fs.existsSync(path.join(entry, command + extension.toLowerCase())) ||
          fs.existsSync(path.join(entry, command + extension.toUpperCase()))) {
        return true;
      }
    }
  }
  return false;
}

function clearFallbackLanguageProviders(): void {
  for (const disposable of fallbackLanguageDisposables) disposable.dispose();
  fallbackLanguageDisposables = [];
}

function registerFallbackLanguageProviders(context: vscode.ExtensionContext): void {
  if (fallbackLanguageDisposables.length > 0) return;
  const selector: vscode.DocumentSelector = { language: 'sura' };
  fallbackLanguageDisposables = [
    vscode.languages.registerCompletionItemProvider(
      selector, new SuraCompletionProvider(), '.', '(', ...COMPLETION_TRIGGER_CHARS),
    vscode.languages.registerHoverProvider(selector, new SuraHoverProvider()),
    vscode.languages.registerSignatureHelpProvider(selector, new SuraSignatureProvider(), '(', ',')
  ];
  context.subscriptions.push(...fallbackLanguageDisposables);
}

function startLanguageFeatures(context: vscode.ExtensionContext): void {
  const enabled = vscode.workspace.getConfiguration('sura').get<boolean>('languageServer.enabled', true);
  const command = enginePath(vscode.window.activeTextEditor?.document.fileName);
  if (!enabled || !executableAvailable(command)) {
    registerFallbackLanguageProviders(context);
    if (enabled) {
      vscode.window.showWarningMessage(
        `Sura language server executable was not found (${command}). Using built-in completion only.`
      );
    }
    return;
  }

  clearFallbackLanguageProviders();
  const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  const serverOptions: ServerOptions = {
    command,
    args: [...configuredLanguageArgs(), '--lsp'],
    transport: TransportKind.stdio,
    options: workspaceRoot ? { cwd: workspaceRoot } : undefined
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'sura' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.sura')
    }
  };

  const client = new LanguageClient(
    'suraLanguageServer',
    'Sura Language Server',
    serverOptions,
    clientOptions
  );
  languageClient = client;
  void client.start().catch((error: unknown) => {
    if (languageClient !== client) return;
    languageClient = undefined;
    registerFallbackLanguageProviders(context);
    vscode.window.showWarningMessage(
      `Sura language server failed to start. Using built-in completion only. ${String(error)}`
    );
  });
}

async function restartLanguageFeatures(context: vscode.ExtensionContext): Promise<void> {
  if (languageClient) {
    await languageClient.stop();
    languageClient = undefined;
  }
  clearFallbackLanguageProviders();
  startLanguageFeatures(context);
}

export function activate(context: vscode.ExtensionContext): void {
  const selector: vscode.DocumentSelector = { language: 'sura' };
  const statusBarActions = new SuraStatusBarActions();

  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider(selector, new SuraRunCodeLensProvider()),
    vscode.commands.registerCommand('sura.createProject', () => createStarterProject()),
    vscode.commands.registerCommand('sura.runPackage', () => runPackageCommand('run')),
    vscode.commands.registerCommand('sura.testPackage', () => runPackageCommand('test')),
    vscode.commands.registerCommand('sura.runFile', () => runSura([])),
    vscode.commands.registerCommand('sura.debugFile', () => debugSura()),
    vscode.commands.registerCommand('sura.runJIT', () => runSura(['--jit'])),
    vscode.commands.registerCommand('sura.runProfile', () => runSura(['--profile'])),
    vscode.commands.registerCommand('sura.traceFile', () => runSura(['--trace'])),
    vscode.commands.registerCommand('sura.restartLanguageServer', () => restartLanguageFeatures(context)),
    vscode.commands.registerCommand('sura.runREPL', () => {
      const term = terminal();
      term.show();
      term.sendText(terminalCommand([enginePath(), ...configuredLanguageArgs(), '--repl']));
    })
  );
  startLanguageFeatures(context);
  statusBarActions.register(context);
}

export async function deactivate(): Promise<void> {
  clearFallbackLanguageProviders();
  if (languageClient) await languageClient.stop();
  languageClient = undefined;
}
