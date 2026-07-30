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

Test(ir_stage14, variables_validation_and_cycles) {
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);

  // 1. Check invalid naming (hyphen)
  const char *invalid_name_yaml =
      "version: 2.0.0\n"
      "name: invalid_name_wf\n"
      "on: { manual: {} }\n"
      "variables:\n"
      "  - name: my-var\n"
      "    expression: \"'val'\"\n"
      "jobs: {}\n";
  WorkflowAST ast1;
  int32_t parse_res = parser_parse_buffer(arena, invalid_name_yaml, strlen(invalid_name_yaml), &ast1);
  if (parse_res == ERR_SUCCESS) {
    int32_t comp_res = compile_workflow(arena, &ast1);
    cr_assert_neq(comp_res, ERR_SUCCESS, "Invalid variable name with hyphen should fail compilation");
  }

  // 2. Check cyclic dependencies
  const char *cyclic_yaml =
      "version: 2.0.0\n"
      "name: cyclic_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: transform\n"
      "    variables:\n"
      "      - name: var_a\n"
      "        expression: \"var_b\"\n"
      "      - name: var_b\n"
      "        expression: \"var_a\"\n"
      "    spec:\n"
      "      expression: \"'done'\"\n";
  WorkflowAST ast2;
  parse_res = parser_parse_buffer(arena, cyclic_yaml, strlen(cyclic_yaml), &ast2);
  if (parse_res == ERR_SUCCESS) {
    int32_t comp_res = compile_workflow(arena, &ast2);
    cr_assert_eq(comp_res, ERR_CYCLIC_DEP, "Variable cyclic dependency should return ERR_CYCLIC_DEP");
  }

  arena_destroy(arena);
}

Test(ir_stage14, variables_execution) {
  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: 2.0.0\n"
      "name: vars_execution_wf\n"
      "on: { manual: {} }\n"
      "variables:\n"
      "  - name: global_const\n"
      "    expression: \"'hello'\"\n"
      "jobs:\n"
      "  job1:\n"
      "    type: transform\n"
      "    variables:\n"
      "      - name: private_var\n"
      "        expression: \"global_const & ' world'\"\n"
      "        visibility: private\n"
      "      - name: public_var\n"
      "        expression: \"private_var & '!'\"\n"
      "        visibility: public\n"
      "    spec:\n"
      "      expression: \"private_var\"\n";

  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jarena, NULL);
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_res = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_res, ERR_SUCCESS, "run_workflow failed with code %d", run_res);

  // Check public variable is exposed at jobs.job1.outputs.public_var
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value outputs_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "outputs", &outputs_obj) && outputs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value pub_var;
  cr_assert(jsonv_obj_get(outputs_obj.as.p, "public_var", &pub_var) && pub_var.tag == JSONV_VAL_STRING);
  cr_assert_str_eq((const char *)pub_var.as.p, "hello world!");

  // Check that private variable is NOT serialized to the jobs context
  Jsonv_Value priv_var;
  cr_assert(!jsonv_obj_get(job1_obj.as.p, "private_var", &priv_var));
  cr_assert(!jsonv_obj_get(outputs_obj.as.p, "private_var", &priv_var));

  arena_destroy(arena);
}

Test(ir_stage14_5, step_outcome_projection) {
  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: 2.0.0\n"
      "name: projection_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: run_plugin\n"
      "        uses: \"test_dynamic_plugin.so\"\n"
      "        outputs:\n"
      "          projected_status: \"outputs.status\"\n"
      "          val: \"outputs.computed_val\"\n";

  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jarena, NULL);
  
  // Set inputs.param_in (needed by the plugin!)
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, inputs_obj, "param_in", jsonv_val_str("test_input"));
  jsonv_obj_set(jarena, root_obj, "inputs", jsonv_val_obj(inputs_obj));
  
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_res = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_res, ERR_SUCCESS, "run_workflow failed with code %d", run_res);

  // Check the outputs in the serialized context_val
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value steps_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value run_plugin_obj;
  cr_assert(jsonv_obj_get(steps_obj.as.p, "run_plugin", &run_plugin_obj) && run_plugin_obj.tag == JSONV_VAL_OBJ);

  // Check projected outputs
  Jsonv_Value outputs_obj;
  cr_assert(jsonv_obj_get(run_plugin_obj.as.p, "outputs", &outputs_obj) && outputs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value status_val;
  cr_assert(jsonv_obj_get(outputs_obj.as.p, "projected_status", &status_val) && status_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq((const char *)status_val.as.p, "success");

  Jsonv_Value val_val;
  cr_assert(jsonv_obj_get(outputs_obj.as.p, "val", &val_val) && val_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq((const char *)val_val.as.p, "Processed: test_input");

  // Check that body and stderr are null (reclaimed / minimized!)
  Jsonv_Value body_val;
  cr_assert(jsonv_obj_get(run_plugin_obj.as.p, "body", &body_val) && body_val.tag == JSONV_VAL_NULL);

  Jsonv_Value stderr_val;
  cr_assert(jsonv_obj_get(run_plugin_obj.as.p, "stderr", &stderr_val) && stderr_val.tag == JSONV_VAL_NULL);

  arena_destroy(arena);
}



