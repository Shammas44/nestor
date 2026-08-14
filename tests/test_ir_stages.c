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
  const char *xml_sample = "<root><name>Alice</name><role>Admin</role></root>";
  Jsonv_Value doc_val;
  int32_t xml_res = xml_to_json(arena, jarena, (StringView){xml_sample, strlen(xml_sample)}, XML_PARKER, &doc_val);
  cr_assert_eq(xml_res, ERR_SUCCESS, "XML transcoding should succeed");
  cr_assert_eq(doc_val.tag, JSONV_VAL_OBJ);

  // 2. CSV Transcoder test
  const char *csv_sample = "id,name,tier\n1,Alice,basic\n2,Bob,enterprise\n";
  Jsonv_Value table_val;
  CsvOptions csv_opts = { .header = true, .delimiter = ',', .relaxed = false };
  int32_t csv_res = csv_to_json(arena, jarena, (StringView){csv_sample, strlen(csv_sample)}, csv_opts, &table_val);
  cr_assert_eq(csv_res, ERR_SUCCESS, "CSV transcoding should succeed");
  cr_assert_eq(table_val.tag, JSONV_VAL_ARRAY);

  // 3. Binary Transcoder test
  const char *bin_sample_b64 = "SGVsbG8="; // "Hello" in base64
  Jsonv_Value bin_val;
  int32_t bin_res = binary_decode(arena, jarena, (StringView){bin_sample_b64, strlen(bin_sample_b64)}, BINARY_BASE64, &bin_val);
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

Test(ir_stage21, bytecode_signature_verification) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);

  const char *yaml_spec = 
      "version: 2.0.0\n"
      "name: test_signature_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps: []\n";

  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml_spec, strlen(yaml_spec), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t dag_res = compile_workflow(arena, &ast);
  cr_assert_eq(dag_res, ERR_SUCCESS);

  const char *tmp_nbc = "/tmp/test_signature_output.nbc";
  int32_t compile_res = bytecode_compile_workflow(arena, &ast, tmp_nbc);
  cr_assert_eq(compile_res, ERR_SUCCESS);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  NVMContext nvm_ctx;
  int32_t nvm_init_res = nvm_init_from_file(&nvm_ctx, arena, jarena, tmp_nbc);
  cr_assert_eq(nvm_init_res, ERR_SUCCESS);
  nvm_close(&nvm_ctx);

  // Corrupt a byte in the Constant Pool/Code area to trigger signature validation failure
  FILE *f = fopen(tmp_nbc, "r+b");
  cr_assert_not_null(f);
  fseek(f, sizeof(NVMHeader) + 5, SEEK_SET);
  uint8_t corrupted_byte = 0xFF;
  fwrite(&corrupted_byte, 1, 1, f);
  fclose(f);

  nvm_init_res = nvm_init_from_file(&nvm_ctx, arena, jarena, tmp_nbc);
  cr_assert_eq(nvm_init_res, ERR_VM_ILLEGAL_INSTRUCTION, "Corrupted binary signature check must fail");
  
  unlink(tmp_nbc);
  arena_destroy(arena);
  /*#endregion*/
}

Test(ir_stage22, plugin_unified_hot_swappability) {
  /*#region*/
  NestorPluginAPI api;
  memset(&api, 0, sizeof(api));

  // 1. Attempt to load the test plugin dynamically
  int32_t rc = plugin_load_dynamic("test_dynamic_plugin", &api);
  cr_assert_eq(rc, 0, "Should load dynamic plugin test_dynamic_plugin successfully");
  cr_assert_not_null(api.execute, "Plugin execute callback must not be null");

  // 2. Load a non-existent plugin and verify it fails gracefully
  NestorPluginAPI dummy_api;
  int32_t fail_rc = plugin_load_dynamic("non_existent_plugin_9999", &dummy_api);
  cr_assert_neq(fail_rc, 0, "Loading non-existent plugin should fail");

  if (api.shutdown) {
    api.shutdown();
  }
  /*#endregion*/
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

Test(ir_stage15, global_provider_configs) {
  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: 2.0.0\n"
      "name: provider_wf\n"
      "on: { manual: {} }\n"
      "providers:\n"
      "  test_dynamic_plugin:\n"
      "    connection_string: \"mysql://host:3306\"\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    variables:\n"
      "      - name: prov_conn\n"
      "        expression: \"providers.test_dynamic_plugin.connection_string\"\n"
      "        visibility: public\n"
      "    steps:\n"
      "      - id: run_prov\n"
      "        provider: \"test_dynamic_plugin.my_op\"\n"
      "        args:\n"
      "          dummy: \"abc\"\n";

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

  // Check that the provider config value was resolved, injected, and evaluated correctly in job variables
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value outputs_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "outputs", &outputs_obj) && outputs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value config_val;
  cr_assert(jsonv_obj_get(outputs_obj.as.p, "prov_conn", &config_val) && config_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq((const char *)config_val.as.p, "mysql://host:3306");

  arena_destroy(arena);
}

Test(ir_stage15_5, step_level_fallbacks) {
  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: 2.0.0\n"
      "name: fallback_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: failed_http\n"
      "        http:\n"
      "          method: \"GET\"\n"
      "          url: \"http://127.0.0.1:1/nonexistent\"\n"
      "        on_error:\n"
      "          fallback:\n"
      "            status: \"offline\"\n"
      "            data: [1, 2, 3]\n";

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

  // Check that the step outcome body was replaced with the fallback object
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value steps_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value failed_http_obj;
  cr_assert(jsonv_obj_get(steps_obj.as.p, "failed_http", &failed_http_obj) && failed_http_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value body_val;
  cr_assert(jsonv_obj_get(failed_http_obj.as.p, "body", &body_val) && body_val.tag == JSONV_VAL_OBJ);

  Jsonv_Value status_val;
  cr_assert(jsonv_obj_get(body_val.as.p, "status", &status_val) && status_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq((const char *)status_val.as.p, "offline");

  Jsonv_Value data_val;
  cr_assert(jsonv_obj_get(body_val.as.p, "data", &data_val) && data_val.tag == JSONV_VAL_ARRAY);

  arena_destroy(arena);
}

Test(ir_stage15_5, state_locking_and_resuming) {
  unlink(".state_wf.tfstate");
  unlink(".state_wf.tfstate.lock");

  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: 2.0.0\n"
      "name: state_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: step_one\n"
      "        uses: \"test_dynamic_plugin\"\n"
      "        with:\n"
      "          param_in: \"first_value\"\n"
      "  job2:\n"
      "    type: wait_signal\n"
      "    depends_on:\n"
      "      - job: job1\n"
      "    spec:\n"
      "      correlation_id: \"inputs.my_corr\"\n"
      "  job3:\n"
      "    type: task\n"
      "    depends_on:\n"
      "      - job: job2\n"
      "    steps:\n"
      "      - id: step_two\n"
      "        uses: \"test_dynamic_plugin\"\n"
      "        with:\n"
      "          param_in: \"second_value\"\n";

  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS);

  // FIRST RUN: wrong correlation ID (should suspend)
  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj1 = jsonv_obj_new(jarena, NULL);
  Jsonv_Obj *inputs_obj1 = jsonv_obj_new(jarena, NULL);
  // Set inputs.param_in (needed by dynamic plugin)
  jsonv_obj_set(jarena, inputs_obj1, "param_in", jsonv_val_str("test_input"));
  jsonv_obj_set(jarena, inputs_obj1, "my_corr", jsonv_val_str("expected_id"));
  jsonv_obj_set(jarena, inputs_obj1, "correlation_id", jsonv_val_str("wrong_id"));
  jsonv_obj_set(jarena, root_obj1, "inputs", jsonv_val_obj(inputs_obj1));
  Jsonv_Value context_val1 = jsonv_val_obj(root_obj1);

  int32_t run_res1 = run_workflow(arena, &ast, &context_val1);
  cr_assert_eq(run_res1, ERR_SUCCESS, "run_workflow 1 failed with code %d", run_res1);

  // Assert that state file exists
  cr_assert(access(".state_wf.tfstate", F_OK) == 0, ".state_wf.tfstate was not written");
  cr_assert(access(".state_wf.tfstate.lock", F_OK) != 0, ".state_wf.tfstate.lock was not cleaned up");

  // LOCK TEST: manual lock acquisition should block second run
  FILE *lock_file = fopen(".state_wf.tfstate.lock", "w");
  cr_assert_not_null(lock_file);
  fprintf(lock_file, "9999");
  fclose(lock_file);

  Jsonv_Obj *root_obj2 = jsonv_obj_new(jarena, NULL);
  Jsonv_Obj *inputs_obj2 = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, inputs_obj2, "param_in", jsonv_val_str("test_input"));
  jsonv_obj_set(jarena, inputs_obj2, "my_corr", jsonv_val_str("expected_id"));
  jsonv_obj_set(jarena, inputs_obj2, "correlation_id", jsonv_val_str("expected_id"));
  jsonv_obj_set(jarena, root_obj2, "inputs", jsonv_val_obj(inputs_obj2));
  Jsonv_Value context_val2 = jsonv_val_obj(root_obj2);

  int32_t run_res2 = run_workflow(arena, &ast, &context_val2);
  cr_assert_eq(run_res2, ERR_LOCKED, "run_workflow should have failed with ERR_LOCKED, got %d", run_res2);

  // Release lock
  unlink(".state_wf.tfstate.lock");

  // SECOND RUN (RESUME): correct correlation ID
  int32_t run_res3 = run_workflow(arena, &ast, &context_val2);
  cr_assert_eq(run_res3, ERR_SUCCESS, "run_workflow 3 failed with code %d", run_res3);

  // Assert tfstate was deleted on completion
  cr_assert(access(".state_wf.tfstate", F_OK) != 0, ".state_wf.tfstate was not cleaned up");

  // Verify that both job1 and job3 executed and their steps outcomes exist in the final context
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj2, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj, job3_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job3", &job3_obj) && job3_obj.tag == JSONV_VAL_OBJ);

  arena_destroy(arena);
}

Test(ir_stage16, data_sources_vs_resources) {
  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: \"2.0.0\"\n"
      "name: cache_split_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  read_job:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: get_step\n"
      "        http:\n"
      "          method: GET\n"
      "          url: \"https://httpbin.org/get\"\n"
      "  write_job:\n"
      "    type: task\n"
      "    depends_on:\n"
      "      - job: read_job\n"
      "    steps:\n"
      "      - id: post_step\n"
      "        http:\n"
      "          method: POST\n"
      "          url: \"https://httpbin.org/post\"\n";

  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS, "compile failed with %d", compile_res);

  JobNode *j = ast.jobs_head;
  bool found_read = false;
  bool found_write = false;
  while (j) {
    if (sv_equals_cstr(j->id, "read_job")) {
      found_read = true;
      cr_assert(j->spec.task.steps_head != NULL);
      cr_assert_eq(j->spec.task.steps_head->is_resource, false, "GET step should not be classified as a resource");
    } else if (sv_equals_cstr(j->id, "write_job")) {
      found_write = true;
      cr_assert(j->spec.task.steps_head != NULL);
      cr_assert_eq(j->spec.task.steps_head->is_resource, true, "POST step should be classified as a resource");
    }
    j = j->next_sorted;
  }
  cr_assert(found_read && found_write);

  arena_destroy(arena);
}

Test(ir_stage16_5, declarative_yaml_providers) {
  // Create providers directory and the test declarative provider yaml file
  system("mkdir -p ./providers");
  FILE *f = fopen("./providers/test_dec.yaml", "w");
  cr_assert_not_null(f);
  const char *prov_yaml =
      "provider: test_dec\n"
      "description: Declarative test helper\n"
      "operations:\n"
      "  run_test:\n"
      "    uses: test_dynamic_plugin.so\n";
  fputs(prov_yaml, f);
  fclose(f);

  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
      "version: \"2.0.0\"\n"
      "name: dec_wf\n"
      "on: { manual: {} }\n"
      "providers:\n"
      "  test_dec: {}\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: step_one\n"
      "        provider: test_dec.run_test\n";

  WorkflowAST ast;
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS, "compile failed with %d", compile_res);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jarena, NULL);
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jarena, NULL);
  jsonv_obj_set(jarena, inputs_obj, "param_in", jsonv_val_str("hello_declarative"));
  jsonv_obj_set(jarena, root_obj, "inputs", jsonv_val_obj(inputs_obj));
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_res = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_res, ERR_SUCCESS, "run_workflow failed with code %d", run_res);

  // Assert step outcome set by test_dynamic_plugin
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value steps_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value step_one_obj;
  cr_assert(jsonv_obj_get(steps_obj.as.p, "step_one", &step_one_obj) && step_one_obj.tag == JSONV_VAL_OBJ);


  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(step_one_obj.as.p, "outputs", &outputs_val) && outputs_val.tag == JSONV_VAL_OBJ);

  Jsonv_Value status_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "status", &status_val) && status_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(status_val.as.p, "success");

  Jsonv_Value computed_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "computed_val", &computed_val) && computed_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(computed_val.as.p, "Processed: hello_declarative");

  arena_destroy(arena);

  // Cleanup files
  unlink("./providers/test_dec.yaml");
  rmdir("./providers");
}

Test(test_ir_new_features, insecure_ssl_option) {
  /*#region*/
  Arena *arena = arena_create(128 * 1024);
  cr_assert_not_null(arena);

  const char *wf_yaml =
      "version: 2.0.0\n"
      "name: insecure_wf\n"
      "on: { manual: {} }\n"
      "providers:\n"
      "  my_prov:\n"
      "    url: https://insecure.com\n"
      "    insecure: true\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: step1\n"
      "        http:\n"
      "          method: GET\n"
      "          url: https://insecure.com\n"
      "          insecure: true\n";

  WorkflowAST ast;
  memset(&ast, 0, sizeof(WorkflowAST));
  int32_t parse_res = parser_parse_buffer(arena, wf_yaml, strlen(wf_yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t comp_res = compile_workflow(arena, &ast);
  cr_assert_eq(comp_res, ERR_SUCCESS);

  // Assert step1 has insecure = true
  cr_assert_not_null(ast.jobs_head);
  cr_assert_eq(ast.jobs_head->type, NODE_TASK);
  StepNode *step = ast.jobs_head->spec.task.steps_head;
  cr_assert_not_null(step);
  cr_assert(step->http.insecure, "HTTP step insecure flag must be compiled to true");

  arena_destroy(arena);
  /*#endregion*/
}

Test(test_ir_new_features, headers_response_retrieval) {
  /*#region*/
  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  transport_mock_clear();
  transport_mock_add_response("http://example.com/api", "GET", 200, "{\"ok\": true}");
  transport_mock_add_header("http://example.com/api", "GET", "X-Custom-Header", "nestor-test");
  transport_mock_add_header("http://example.com/api", "GET", "Content-Type", "application/json");

  const char *wf_yaml =
      "version: 2.0.0\n"
      "name: headers_wf\n"
      "on: { manual: {} }\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: step_one\n"
      "        http:\n"
      "          method: GET\n"
      "          url: http://example.com/api\n"
      "        outputs:\n"
      "          token: \"headers.X-Custom-Header\"\n"
      "          content_type: \"headers.Content-Type\"\n";

  WorkflowAST ast;
  memset(&ast, 0, sizeof(WorkflowAST));
  int32_t parse_res = parser_parse_buffer(arena, wf_yaml, strlen(wf_yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t comp_res = compile_workflow(arena, &ast);
  cr_assert_eq(comp_res, ERR_SUCCESS);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jarena, NULL);
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  Transport *transport = transport_mock_new(arena);
  cr_assert_not_null(transport);

  int32_t run_res = run_workflow_opt(arena, &ast, &context_val, transport);
  cr_assert_eq(run_res, ERR_SUCCESS);

  // Assert headers are in outcomes
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value steps_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value step_one_obj;
  cr_assert(jsonv_obj_get(steps_obj.as.p, "step_one", &step_one_obj) && step_one_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value headers_val;
  cr_assert(jsonv_obj_get(step_one_obj.as.p, "headers", &headers_val) && headers_val.tag == JSONV_VAL_OBJ);

  Jsonv_Value custom_h;
  cr_assert(jsonv_obj_get(headers_val.as.p, "X-Custom-Header", &custom_h) && custom_h.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(custom_h.as.p, "nestor-test");

  // Assert outputs are correctly projected
  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(step_one_obj.as.p, "outputs", &outputs_val) && outputs_val.tag == JSONV_VAL_OBJ);

  Jsonv_Value token_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "token", &token_val) && token_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(token_val.as.p, "nestor-test");

  Jsonv_Value ct_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "content_type", &ct_val) && ct_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(ct_val.as.p, "application/json");

  transport->ops->destroy(transport);
  arena_destroy(arena);
  /*#endregion*/
}

Test(test_ir_new_features, declarative_http_provider_outputs) {
  /*#region*/
  system("mkdir -p ./providers");
  FILE *f = fopen("./providers/test_dec_http.yaml", "w");
  cr_assert_not_null(f);
  const char *prov_yaml =
      "provider: test_dec_http\n"
      "description: HTTP provider with outputs\n"
      "configuration:\n"
      "  api_url: \"http://example.com/api\"\n"
      "operations:\n"
      "  get_user:\n"
      "    uses: http\n"
      "    args:\n"
      "      method: GET\n"
      "      url: \"${{ configuration.api_url }}/user\"\n"
      "    outputs:\n"
      "      user_id: \"body.id\"\n"
      "      auth_token: \"headers.X-token\"\n";
  fputs(prov_yaml, f);
  fclose(f);

  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  transport_mock_clear();
  transport_mock_add_response("http://example.com/api/user", "GET", 200, "{\"id\": \"usr_999\"}");
  transport_mock_add_header("http://example.com/api/user", "GET", "X-token", "tkn_12345");

  const char *yaml =
      "version: \"2.0.0\"\n"
      "name: dec_http_wf\n"
      "on: { manual: {} }\n"
      "providers:\n"
      "  test_dec_http:\n"
      "    api_url: \"http://example.com/api\"\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: get_profile\n"
      "        provider: test_dec_http.get_user\n";

  WorkflowAST ast;
  memset(&ast, 0, sizeof(WorkflowAST));
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jarena, NULL);
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  Transport *transport = transport_mock_new(arena);
  cr_assert_not_null(transport);

  int32_t run_res = run_workflow_opt(arena, &ast, &context_val, transport);
  cr_assert_eq(run_res, ERR_SUCCESS);

  // Assert step outcome
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value steps_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value get_profile_obj;
  cr_assert(jsonv_obj_get(steps_obj.as.p, "get_profile", &get_profile_obj) && get_profile_obj.tag == JSONV_VAL_OBJ);

  // Assert step outputs contain user_id and auth_token from provider mapping
  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(get_profile_obj.as.p, "outputs", &outputs_val) && outputs_val.tag == JSONV_VAL_OBJ);

  Jsonv_Value user_id_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "user_id", &user_id_val) && user_id_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(user_id_val.as.p, "usr_999");

  Jsonv_Value auth_token_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "auth_token", &auth_token_val) && auth_token_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(auth_token_val.as.p, "tkn_12345");

  transport->ops->destroy(transport);
  arena_destroy(arena);

  // Cleanup files
  unlink("./providers/test_dec_http.yaml");
  rmdir("./providers");
  /*#endregion*/
}

Test(test_ir_new_features, declarative_http_provider_step_outputs) {
  /*#region*/
  system("mkdir -p ./providers");
  FILE *f = fopen("./providers/test_dec_http.yaml", "w");
  cr_assert_not_null(f);
  const char *prov_yaml =
      "provider: test_dec_http\n"
      "description: HTTP provider with outputs\n"
      "configuration:\n"
      "  api_url: \"http://example.com/api\"\n"
      "operations:\n"
      "  get_user:\n"
      "    uses: http\n"
      "    args:\n"
      "      method: GET\n"
      "      url: \"${{ configuration.api_url }}/user\"\n"
      "    outputs:\n"
      "      user_id: \"body.id\"\n"
      "      auth_token: \"headers.X-token\"\n";
  fputs(prov_yaml, f);
  fclose(f);

  Arena *arena = arena_create(256 * 1024);
  cr_assert_not_null(arena);

  transport_mock_clear();
  transport_mock_add_response("http://example.com/api/user", "GET", 200, "{\"id\": \"usr_999\"}");
  transport_mock_add_header("http://example.com/api/user", "GET", "X-token", "tkn_12345");

  const char *yaml =
      "version: \"2.0.0\"\n"
      "name: dec_http_wf\n"
      "on: { manual: {} }\n"
      "providers:\n"
      "  test_dec_http:\n"
      "    api_url: \"http://example.com/api\"\n"
      "jobs:\n"
      "  job1:\n"
      "    type: task\n"
      "    steps:\n"
      "      - id: get_profile\n"
      "        provider: test_dec_http.get_user\n"
      "        outputs:\n"
      "          wf_token: \"outputs.auth_token\"\n"
      "          wf_user: \"outputs.user_id\"\n";

  WorkflowAST ast;
  memset(&ast, 0, sizeof(WorkflowAST));
  int32_t parse_res = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_res, ERR_SUCCESS);

  int32_t compile_res = compile_workflow(arena, &ast);
  cr_assert_eq(compile_res, ERR_SUCCESS);

  Jsonv_Arena *jarena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jarena, NULL);
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  Transport *transport = transport_mock_new(arena);
  cr_assert_not_null(transport);

  int32_t run_res = run_workflow_opt(arena, &ast, &context_val, transport);
  cr_assert_eq(run_res, ERR_SUCCESS);

  // Assert step outcome
  Jsonv_Value jobs_obj;
  cr_assert(jsonv_obj_get(root_obj, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value job1_obj;
  cr_assert(jsonv_obj_get(jobs_obj.as.p, "job1", &job1_obj) && job1_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value steps_obj;
  cr_assert(jsonv_obj_get(job1_obj.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ);

  Jsonv_Value get_profile_obj;
  cr_assert(jsonv_obj_get(steps_obj.as.p, "get_profile", &get_profile_obj) && get_profile_obj.tag == JSONV_VAL_OBJ);

  // Assert step outputs contain wf_token and wf_user from step level mapping
  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(get_profile_obj.as.p, "outputs", &outputs_val) && outputs_val.tag == JSONV_VAL_OBJ);

  Jsonv_Value wf_user_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "wf_user", &wf_user_val) && wf_user_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(wf_user_val.as.p, "usr_999");

  Jsonv_Value wf_token_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "wf_token", &wf_token_val) && wf_token_val.tag == JSONV_VAL_STRING);
  cr_assert_str_eq(wf_token_val.as.p, "tkn_12345");

  transport->ops->destroy(transport);
  arena_destroy(arena);

  // Cleanup files
  unlink("./providers/test_dec_http.yaml");
  rmdir("./providers");
  /*#endregion*/
}






