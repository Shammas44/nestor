Nestor Orchestration Engine - Examples and Testing Scenarios

This directory contains eighteen scenarios showing the configuration language and capabilities of the Nestor CLI execution engine.

================================================================================
EASY TESTING: RUN SCENARIOS VIA SHELL SCRIPT
================================================================================
You can use the helper script in the root directory to automatically build the binary, start the mock server, and launch any of the scenarios:

  ./run_scenario.sh

================================================================================
PREPARATION: START THE MOCK SERVER (MANUALLY)
================================================================================
To test HTTP-related workflows manually, start the built-in FastAPI mock server from its virtual environment:

./mock_server/.venv/bin/python mock_server/main.py 8080

Keep this server running in a separate terminal during HTTP testing.

================================================================================
SCENARIO 1: Basic HTTP Orchestration
================================================================================
- File: examples/01_basic_http.json
- Purpose: Demonstrates native HTTP POST capabilities, injecting variables into request headers and body, and reading outcomes.
- How to Run:
  
  cat examples/01_basic_http.json | ./bin/main user_id=admin_user tier=enterprise | jq

- Expected Outcome:
  Returns status 200, containing the body returned by the mock server (which accepted the POST call on `/api/provision`).

================================================================================
SCENARIO 2: Custom Subprocess Plugin Execution
================================================================================
- File: examples/02_plugin_pipeline.json
- Purpose: Demonstrates running standalone plugins via fork & execvp, routing variables directly into the plugin's configuration context, and piping output.
- How to Run:
  
  # Ensure the mock plugin binary is built first (make main does this automatically)
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
  
  # Run the workflow
  ENV_NAME=production-cluster cat examples/02_plugin_pipeline.json | ./bin/main user_id=developer_99 | jq

- Expected Outcome:
  The mock plugin is successfully executed, reads input data, and produces exit code 0 and `"plugin_output": "success"`.

================================================================================
SCENARIO 3: Enterprise Deployment Gateway Graph
================================================================================
- File: examples/03_enterprise_deploy.json
- Purpose: Demonstrates the complex DAG structures of Nestor. Models structural control-flow nodes (if, fork, join, loops) which synchronize parallel branches.
- Purpose of each block:
  - 'validate_payload': Task querying `/api/validate`.
  - 'check_tier': If node branching based on inputs.tier.
  - 'deploy_standard': Downstream path for standard users.
  - 'split_deployments': Fork node splitting path concurrently into multiple regions.
  - 'deploy_us' & 'deploy_eu': Concurrent deployment tasks.
  - 'sync_deployments': Join node synchronizing the concurrent tasks.
  - 'notify_success': Merges execution back to complete the workflow.
- Status: Fully supported.
- How to Run:
  
  cat examples/03_enterprise_deploy.json | ./bin/main user_id=admin_user tier=enterprise | jq

================================================================================
SCENARIO 4: Multi-Step JSONata Transformations
================================================================================
- File: examples/04_multistep_jsonata.json
- Purpose: Demonstrates a sequential multi-step task sharing step outcomes and executing complex JSONata evaluations (string concatenations, object navigations, casing transformations, environment maps).
- How to Run:
  
  # Ensure the mock Python HTTP server is running on port 8080 (see above)
  # Run the pipeline passing inputs and environment
  ENV_NAME=testing-racks cat examples/04_multistep_jsonata.json | ./bin/main user_id=john_doe_pro | jq

- Expected Outcome:
  Returns status 200, outputting the result of the steps. The second step ('transform_and_log') will contain:
  "plugin_output": "success"
  And prints/logs:
  "PROVISION_SUCCESS: res_12345 [State: accepted] - Triggered by JOHN_DOE_PRO under env: testing-racks"

================================================================================
SCENARIO 5: Concurrency and Early Resuming Join Barriers
================================================================================
- File: examples/05_concurrency_resume.json
- Purpose: Demonstrates high-performance parallel execution using curl_multi and early resuming join nodes with strategy 'any'.
- How to Run:
  
  # Ensure the mock Python HTTP server is running on port 8080 (see above)
  cat examples/05_concurrency_resume.json | ./bin/main | jq

- Expected Outcome:
  Under the hood, the scheduler triggers both 'fast_job' (takes 0.1s) and 'slow_job' (takes 2.0s) concurrently.
  Because 'join_barrier' is configured with 'strategy: any', the downstream task 'downstream_task' is executed and completes immediately after 'fast_job' finishes (in ~0.1s), without waiting for the slow 2-second job.

================================================================================
SCENARIO 6: Secret Management and Log/Payload Redaction
================================================================================
- File: examples/06_secrets_redaction.json
- Purpose: Demonstrates native secrets processing via Aho-Corasick. Secrets are resolved on-demand and fully redacted from both observability logs and step outputs (response bodies/payloads).
- How to Run:
  
  # Inject secrets using NESTOR_SECRET_ prefixed environment variables
  NESTOR_SECRET_API_KEY=super_secret_token_123 NESTOR_SECRET_DB_PASSWORD=my_private_passphrase cat examples/06_secrets_redaction.json | ./bin/main | jq

- Expected Outcome:
  1. Observability logs automatically print redacted values (`***`) instead of raw secret values.
  2. The output workspace JSON printed at the end contains the redacted values `***` for headers and response bodies containing matching secrets:
     "body": "{\n  \"path\": \"/api/deploy/standard\",\n  \"method\": \"POST\",\n  \"received_body\": {\n    \"password\": \"***\",\n    \"action\": \"deploy\"\n  }\n}"

================================================================================
SCENARIO 7: Conditional Execution (If/Switch Nodes)
================================================================================
- File: examples/07_conditionals.json
- Purpose: Demonstrates branching orchestration workflows using conditional "if" and multi-branch "switch" job types based on expression evaluations.
- How to Run:
  
  cat examples/07_conditionals.json | ./bin/main user_id=admin_user tier=enterprise region=eu

- Expected Outcome:
  1. The "check_tier" job evaluates if `tier` is 'enterprise'. Since it is, the "enterprise_deploy" job is triggered, and "standard_deploy" is marked as SKIPPED.
  2. The "region_switch" job evaluates `inputs.region` and matches the 'eu' case, triggering "deploy_eu" and marking "deploy_us" and "deploy_fallback" as SKIPPED.

================================================================================
SCENARIO 8: Loop Iterations (While / For-Each Loops)
================================================================================
- File: examples/08_loops.json
- Purpose: Demonstrates loop nodes supporting automatic index counting for "while" loops and list iterator element binding for "for_each" loops.
- How to Run:
  
  cat examples/08_loops.json | ./bin/main

- Expected Outcome:
  1. The "while_loop" runs the delay step three times, incrementing `index` (0, 1, 2) until `index < 3` evaluates to false.
  2. The "for_each_loop" iterates through the list `['eu', 'us']` mapping each string to `item` and calling `/api/deploy/eu` and `/api/deploy/us` dynamically.

================================================================================
SCENARIO 9: SWAPI Character Deep Dive
================================================================================
- File: examples/swapi/01_character_deep_dive.json
- Purpose: Queries a character, resolves home planet URL dynamically, queries the planet, and branches conditional execution based on the planet's population.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/01_character_deep_dive.json | ./bin/main | jq
- Expected Outcome:
  Returns status 200, outputting results of the conditional branch (e.g., "log_heavy" step executes).

================================================================================
SCENARIO 10: SWAPI Starship Fleet Concurrency
================================================================================
- File: examples/swapi/02_starship_fleet_concurrency.json
- Purpose: Forks execution to concurrently query multiple starships (X-wing and Millennium Falcon), wait for all results, and calculate total stats.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/02_starship_fleet_concurrency.json | ./bin/main | jq
- Expected Outcome:
  Returns status 200, with outputs of concurrent starships compiled inside the final task.

================================================================================
SCENARIO 11: SWAPI Planet Colonization Loop
================================================================================
- File: examples/swapi/03_planet_colonization_loop.json
- Purpose: Performs a `for_each` loop over a list of planet IDs, querying each planet and validating its climate and gravity.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/03_planet_colonization_loop.json | ./bin/main planet_ids='["1", "2"]' | jq
- Expected Outcome:
  Iterates over both planet 1 and 2, printing evaluation status for each.

================================================================================
SCENARIO 12: SWAPI Film Character Association
================================================================================
- File: examples/swapi/04_film_character_association.json
- Purpose: Fetches film metadata, loops over character references, and runs validation plugins.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/04_film_character_association.json | ./bin/main character_ids='["1", "2"]' | jq
- Expected Outcome:
  Retrieves film title and loops over character profiles.

================================================================================
SCENARIO 13: SWAPI API Rate Limiter Retry
================================================================================
- File: examples/swapi/05_api_rate_limiter_retry.json
- Purpose: Queries a starship while utilizing timeout limits and backoff-retry strategies to handle potential rate-limiting.
- How to Run:
  cat examples/swapi/05_api_rate_limiter_retry.json | ./bin/main | jq
- Expected Outcome:
  Completes successfully, retrying the HTTP request if rate-limits are encountered.

================================================================================
SCENARIO 14: SWAPI Secret Authorized Proxy
================================================================================
- File: examples/swapi/06_secret_authorized_proxy.json
- Purpose: Makes authenticated HTTP queries to a SWAPI proxy using secrets.
- How to Run:
  NESTOR_SECRET_SWAPI_PROXY_KEY=my-proxy-token-123 cat examples/swapi/06_secret_authorized_proxy.json | ./bin/main | jq
- Expected Outcome:
  Queries the proxy, inserting the secret key into the Authorization header dynamically.

================================================================================
SCENARIO 15: SWAPI Observability Redaction
================================================================================
- File: examples/swapi/07_observability_redaction.json
- Purpose: Demonstrates logs and payloads redaction of sensitive SWAPI names and credentials using Aho-Corasick.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  NESTOR_SECRET_SWAPI_KEY=my-auth-key-123 cat examples/swapi/07_observability_redaction.json | ./bin/main | jq
- Expected Outcome:
  Executes, writing outcomes to a plugin step while redacting the secret values (`***`) in the output and logs.

================================================================================
SCENARIO 16: SWAPI Conditional Species Branching
================================================================================
- File: examples/swapi/08_conditional_species_branching.json
- Purpose: Fetches a character and uses a multi-conditional `switch` node to branch execution based on height.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/08_conditional_species_branching.json | ./bin/main | jq
- Expected Outcome:
  Evaluates the switch cases and routes execution to the appropriate species/height log job.

================================================================================
SCENARIO 17: SWAPI Async Wait Timer
================================================================================
- File: examples/swapi/09_async_wait_timer.json
- Purpose: Pauses execution during a fueling sequence for 1 second using the `wait_timer` block.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/09_async_wait_timer.json | ./bin/main | jq
- Expected Outcome:
  Fetches the starship, waits exactly 1 second, and then completes the launch check.

================================================================================
SCENARIO 18: SWAPI Coordinated Multistage Orchestration
================================================================================
- File: examples/swapi/10_coordinated_multistage_orchestration.json
- Purpose: A large coordinated pipeline combining forks, joins, loops, and http queries.
- Pre-requisite: Compile the mock plugin binary:
  mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
- How to Run:
  cat examples/swapi/10_coordinated_multistage_orchestration.json | ./bin/main planet_ids='["1", "2"]' | jq
- Expected Outcome:
  Runs the multistage DAG, fork-joining loop tasks and character queries successfully.




