#include <criterion/criterion.h>
#include "types.h"
#include "evaluator.h"
#include <string.h>
#include <stdio.h>

static void *test_jsonv_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}
static void test_jsonv_dummy(void *user_data) { /*#region*/ (void)user_data; /*#endregion*/ }
static void test_jsonv_dummy_to(void *user_data, size_t keep_size) { /*#region*/ (void)user_data; (void)keep_size; /*#endregion*/ }

static const Jsonv_Arena_Ops test_jsonv_ops = {
  .alloc = test_jsonv_alloc,
  .reset = test_jsonv_dummy,
  .reset_to = test_jsonv_dummy_to,
  .destroy = test_jsonv_dummy
};

Test(transcoder, csv_to_json_standard) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);
  cr_assert_not_null(jarena);

  // 1. With headers
  const char *csv = "id,name,value\n1,Alice,100\n2,Bob,200\n";
  Jsonv_Value out = jsonv_val_undefined();
  CsvOptions opts = { .header = true, .delimiter = ',', .relaxed = false };
  int32_t status = csv_to_json(arena, jarena, (StringView){csv, strlen(csv)}, opts, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out.tag, JSONV_VAL_ARRAY);
  cr_assert_eq(jsonv_arr_length(out.as.p), 2);

  Jsonv_Value r1;
  cr_assert(jsonv_arr_get(out.as.p, 0, &r1));
  cr_assert_eq(r1.tag, JSONV_VAL_OBJ);
  Jsonv_Value r1_id;
  cr_assert(jsonv_obj_get(r1.as.p, "id", &r1_id));
  cr_assert_str_eq((const char *)r1_id.as.p, "1");

  // 2. Without headers
  opts.header = false;
  status = csv_to_json(arena, jarena, (StringView){csv, strlen(csv)}, opts, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out.tag, JSONV_VAL_ARRAY);
  cr_assert_eq(jsonv_arr_length(out.as.p), 3); // 3 rows, first is headers row

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, csv_to_json_quoted_and_delimiter) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  // Semicolon delimiter, quoted fields with commas and escaped quotes
  const char *csv = "name;msg\nAlice;\"Hello, \"\"World\"\"!\"\nBob;\"Line1\nLine2\"\n";
  CsvOptions opts = { .header = true, .delimiter = ';', .relaxed = false };
  Jsonv_Value out = jsonv_val_undefined();
  int32_t status = csv_to_json(arena, jarena, (StringView){csv, strlen(csv)}, opts, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(jsonv_arr_length(out.as.p), 2);

  Jsonv_Value r1, r1_msg;
  cr_assert(jsonv_arr_get(out.as.p, 0, &r1));
  cr_assert(jsonv_obj_get(r1.as.p, "msg", &r1_msg));
  cr_assert_str_eq((const char *)r1_msg.as.p, "Hello, \"World\"!");

  Jsonv_Value r2, r2_msg;
  cr_assert(jsonv_arr_get(out.as.p, 1, &r2));
  cr_assert(jsonv_obj_get(r2.as.p, "msg", &r2_msg));
  cr_assert_str_eq((const char *)r2_msg.as.p, "Line1\nLine2");

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, xml_to_json_conventions) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  const char *xml = "<book id=\"123\"><title>My Title</title><author>Author A</author><author>Author B</author></book>";
  Jsonv_Value out = jsonv_val_undefined();

  // 1. Parker
  int32_t status = xml_to_json(arena, jarena, (StringView){xml, strlen(xml)}, XML_PARKER, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out.tag, JSONV_VAL_OBJ);
  Jsonv_Value book_val;
  cr_assert(jsonv_obj_get(out.as.p, "book", &book_val));
  Jsonv_Value title_val;
  cr_assert(jsonv_obj_get(book_val.as.p, "title", &title_val));
  cr_assert_str_eq((const char *)title_val.as.p, "My Title");
  Jsonv_Value author_val;
  cr_assert(jsonv_obj_get(book_val.as.p, "author", &author_val));
  cr_assert_eq(author_val.tag, JSONV_VAL_ARRAY); // repeated siblings folded

  // 2. BadgerFish
  status = xml_to_json(arena, jarena, (StringView){xml, strlen(xml)}, XML_BADGERFISH, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert(jsonv_obj_get(out.as.p, "book", &book_val));
  Jsonv_Value id_val;
  cr_assert(jsonv_obj_get(book_val.as.p, "@id", &id_val));
  cr_assert_str_eq((const char *)id_val.as.p, "123");

  // 3. JsonML
  status = xml_to_json(arena, jarena, (StringView){xml, strlen(xml)}, XML_JSONML, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out.tag, JSONV_VAL_ARRAY);
  Jsonv_Value tag_val;
  cr_assert(jsonv_arr_get(out.as.p, 0, &tag_val));
  cr_assert_str_eq((const char *)tag_val.as.p, "book");

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, form_to_json_and_back) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  const char *form = "a=1&b=hello+world&c=%E2%9C%93"; // c = utf-8 checkmark
  Jsonv_Value out = jsonv_val_undefined();
  int32_t status = form_to_json(arena, jarena, (StringView){form, strlen(form)}, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out.tag, JSONV_VAL_OBJ);

  Jsonv_Value b_val, c_val;
  cr_assert(jsonv_obj_get(out.as.p, "b", &b_val));
  cr_assert_str_eq((const char *)b_val.as.p, "hello world");
  cr_assert(jsonv_obj_get(out.as.p, "c", &c_val));
  cr_assert_str_eq((const char *)c_val.as.p, "✓");

  // Format back to form-urlencoded
  StringView back = {NULL, 0};
  status = json_to_form(arena, out, &back);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert(back.length > 0);
  
  // Re-parse to verify round-trip
  Jsonv_Value out2 = jsonv_val_undefined();
  status = form_to_json(arena, jarena, back, &out2);
  cr_assert_eq(status, ERR_SUCCESS);
  Jsonv_Value b2_val;
  cr_assert(jsonv_obj_get(out2.as.p, "b", &b2_val));
  cr_assert_str_eq((const char *)b2_val.as.p, "hello world");

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, binary_encode_decode) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  // 1. Base64
  const char *raw = "Nestor Orchestration Engine";
  StringView b64_encoded = {NULL, 0};
  int32_t status = binary_encode(arena, (StringView){raw, strlen(raw)}, BINARY_BASE64, &b64_encoded);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Value decoded_val;
  status = binary_decode(arena, jarena, b64_encoded, BINARY_BASE64, &decoded_val);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(decoded_val.tag, JSONV_VAL_STRING);
  cr_assert_str_eq((const char *)decoded_val.as.p, raw);

  // 2. Hex
  StringView hex_encoded = {NULL, 0};
  status = binary_encode(arena, (StringView){raw, strlen(raw)}, BINARY_HEX, &hex_encoded);
  cr_assert_eq(status, ERR_SUCCESS);

  status = binary_decode(arena, jarena, hex_encoded, BINARY_HEX, &decoded_val);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_str_eq((const char *)decoded_val.as.p, raw);

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, yaml_to_json_parse) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  const char *yaml = "name: workflow\nsteps:\n  - id: step1\n    uses: plugin\n";
  Jsonv_Value out = jsonv_val_undefined();
  int32_t status = yaml_to_json(arena, jarena, (StringView){yaml, strlen(yaml)}, &out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out.tag, JSONV_VAL_OBJ);

  Jsonv_Value steps;
  cr_assert(jsonv_obj_get(out.as.p, "steps", &steps));
  cr_assert_eq(steps.tag, JSONV_VAL_ARRAY);

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, json_formatters) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  // 1. JSON to CSV
  Jsonv_Arr *arr = jsonv_arr_new(jarena);
  Jsonv_Obj *o1 = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, o1, "k1", jsonv_val_str("v1"));
  jsonv_obj_set(jarena, o1, "k2", jsonv_val_str("v2"));
  jsonv_arr_set(jarena, arr, 0, jsonv_val_obj(o1));
  
  StringView csv_out = {NULL, 0};
  CsvOptions opts = { .header = true, .delimiter = ',', .relaxed = false };
  int32_t status = json_to_csv(arena, jsonv_val_arr(arr), opts, &csv_out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert(strstr(csv_out.data, "k1,k2") != NULL);
  cr_assert(strstr(csv_out.data, "v1,v2") != NULL);

  // 2. JSON to XML (Parker)
  Jsonv_Obj *root = jsonv_obj_new(jarena, NULL);
  Jsonv_Obj *book = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, book, "title", jsonv_val_str("Pride and Prejudice"));
  jsonv_obj_set(jarena, root, "book", jsonv_val_obj(book));

  StringView xml_out = {NULL, 0};
  status = json_to_xml(arena, jsonv_val_obj(root), XML_PARKER, &xml_out);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert(strstr(xml_out.data, "<book><title>Pride and Prejudice</title></book>") != NULL);

  arena_destroy(arena);
  /*#endregion*/
}

Test(transcoder, jsonata_integration) {
  /*#region*/
  Arena *arena = arena_create(256 * 1024);
  Jsonv_Arena *jarena = jsonv_arena_new_custom(&test_jsonv_ops, arena);

  // Parse CSV via JSONata expression
  const char *expr_str = "$csvParse(csv_raw, {'header': true}).name";
  
  Jsonv_Obj *ctx_obj = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, ctx_obj, "csv_raw", jsonv_val_str("id,name\n1,Alice\n2,Bob\n"));
  
  Jsonv_Value out_val = jsonv_val_undefined();
  int32_t status = evaluate_expression(arena, (StringView){expr_str, strlen(expr_str)}, jarena, jsonv_val_obj(ctx_obj), &out_val);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(out_val.tag, JSONV_VAL_ARRAY);
  cr_assert_eq(jsonv_arr_length(out_val.as.p), 2);
  
  Jsonv_Value name1;
  cr_assert(jsonv_arr_get(out_val.as.p, 0, &name1));
  cr_assert_str_eq((const char *)name1.as.p, "Alice");

  // Parse XML via JSONata expression
  const char *expr_xml = "$xmlParse(xml_raw, 'badgerfish').root.\"@id\"";
  Jsonv_Obj *ctx_xml = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, ctx_xml, "xml_raw", jsonv_val_str("<root id=\"abc\"></root>"));
  status = evaluate_expression(arena, (StringView){expr_xml, strlen(expr_xml)}, jarena, jsonv_val_obj(ctx_xml), &out_val);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_str_eq((const char *)out_val.as.p, "abc");

  arena_destroy(arena);
  /*#endregion*/
}
