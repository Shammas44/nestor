#include <criterion/criterion.h>
#include <criterion/logging.h>
#include "nestor.h"
#include "parser.h"
#include <stdio.h>
#include <string.h>


Test(ir_stage9, provider_spi_basics) {
  Arena *arena = arena_create(64 * 1024);
  cr_assert_not_null(arena);

  Provider p = {0};
  int32_t res = provider_load(arena, "./bin/non_existent_provider.so", &p);
  cr_assert_neq(res, ERR_SUCCESS, "Expected failure loading invalid provider library");

  arena_destroy(arena);
}

Test(ir_stage9_5, workspace_loader_and_cycle_check) {
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);

  WorkspaceMap map = {0};
  int32_t load_res = workspace_load_directory(arena, "./examples", &map);
  cr_assert_eq(load_res, ERR_SUCCESS, "Workspace directory load should succeed");

  arena_destroy(arena);
}

Test(ir_stage10, transcoders) {
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);

  WorkflowAST ast;
  const char *minimal_yaml = "version: 2.0.0\nname: test_transcoder\non: { manual: {} }\njobs: {}\n";
  int32_t parse_res = parser_parse_buffer(arena, minimal_yaml, strlen(minimal_yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);
  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);


  // 1. XML Transcoder test
  const char *xml_sample = "<name>Alice</name><role>Admin</role>";
  Jsonv_Value doc_val;
  int32_t xml_res = transcode_xml_to_document(arena, jarena, xml_sample, strlen(xml_sample), &doc_val);
  cr_assert_eq(xml_res, ERR_SUCCESS, "XML transcoding should succeed");
  cr_assert_eq(doc_val.tag, JSONV_VAL_OBJ);

  // 2. CSV Transcoder test
  const char *csv_sample = "id,name,tier\n1,Alice,basic\n2,Bob,enterprise\n";
  Jsonv_Value table_val;
  int32_t csv_res = transcode_csv_to_table(arena, jarena, csv_sample, strlen(csv_sample), ',', &table_val);
  cr_assert_eq(csv_res, ERR_SUCCESS, "CSV transcoding should succeed");
  cr_assert_eq(table_val.tag, JSONV_VAL_ARRAY);

  // 3. Binary Transcoder test
  uint8_t bin_sample[] = { 0x48, 0x65, 0x6C, 0x6C, 0x6F }; // "Hello"
  Jsonv_Value bin_val;
  int32_t bin_res = transcode_binary_to_value(arena, jarena, bin_sample, 5, &bin_val);
  cr_assert_eq(bin_res, ERR_SUCCESS, "Binary transcoding should succeed");
  cr_assert_eq(bin_val.tag, JSONV_VAL_STRING);

  arena_destroy(arena);
}

Test(ir_stage11_5_and_11_7, bytecode_compiler_and_nvm_execution) {
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);

  const char *yaml_spec = 
      "version: 2.0.0\n"
      "name: test_bytecode_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps: []\n";



  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml_spec, strlen(yaml_spec), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS, "AST parsing should succeed");

  int32_t dag_res = compile_workflow(arena, &ast);
  cr_assert_eq(dag_res, ERR_SUCCESS, "DAG compilation should succeed");

  const char *tmp_nbc = "/tmp/test_output.nbc";
  int32_t compile_res = bytecode_compile_workflow(arena, &ast, tmp_nbc);
  cr_assert_eq(compile_res, ERR_SUCCESS, "Bytecode compilation to NBC should succeed");


  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  NVMContext nvm_ctx;

  int32_t nvm_init_res = nvm_init_from_file(&nvm_ctx, arena, jarena, tmp_nbc);
  cr_assert_eq(nvm_init_res, ERR_SUCCESS, "NVM initialization from mapped NBC file should succeed");
  cr_assert_eq(memcmp(nvm_ctx.header->magic, "NEST", 4), 0, "Header magic must match NEST");

  int32_t nvm_exec_res = nvm_execute_loop(&nvm_ctx);
  cr_assert_eq(nvm_exec_res, ERR_SUCCESS, "NVM execution loop should succeed");

  nvm_close(&nvm_ctx);
  arena_destroy(arena);
}


