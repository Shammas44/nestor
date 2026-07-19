#!/bin/bash

# Ensure we are in the project root
cd "$(dirname "$0")"

# 1. Build binary if missing
if [ ! -f "bin/main" ]; then
    echo "=== Building main CLI executable ==="
    OPTION=dev make main
    if [ $? -ne 0 ]; then
        echo "Build failed!"
        exit 1
    fi
    echo ""
fi

# 2. Check and start mock server if missing
nc -z 127.0.0.1 8080 >/dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "=== Starting Mock HTTP Server on port 8080 ==="
    if [ -f "./mock_server/.venv/bin/python" ]; then
        ./mock_server/.venv/bin/python mock_server/main.py 8080 > mock_server.log 2>&1 &
        MOCK_PID=$!
        echo $MOCK_PID > .mock_server.pid
        # Wait for server to start
        sleep 1.5
        echo "Mock server started (PID: $MOCK_PID). Log at mock_server.log"
    else
        echo "Warning: Python virtual environment not found at ./mock_server/.venv/. Please start mock server manually."
    fi
    echo ""
else
    # Try finding any running mock server and save its PID if not saved
    if [ ! -f ".mock_server.pid" ]; then
        MOCK_PID=$(pgrep -f "mock_server/main.py")
        if [ -n "$MOCK_PID" ]; then
            echo $MOCK_PID > .mock_server.pid
        fi
    fi
fi

stop_mock_server() {
    if [ -f ".mock_server.pid" ]; then
        PID=$(cat .mock_server.pid)
        if kill -0 $PID >/dev/null 2>&1; then
            kill $PID
            echo "Mock server (PID: $PID) terminated."
        else
            echo "Mock server process not running."
        fi
        rm -f .mock_server.pid
    else
        PID=$(pgrep -f "mock_server/main.py")
        if [ -n "$PID" ]; then
            kill $PID
            echo "Mock server (PID: $PID) terminated."
        else
            echo "No mock server process found."
        fi
    fi
}

while true; do
    echo "=============================================="
    echo " Nestor CLI Scenario Runner"
    echo "=============================================="
    echo "1) Scenario 1: Basic HTTP Orchestration (01_basic_http.json)"
    echo "2) Scenario 2: Custom Subprocess Plugin Execution (02_plugin_pipeline.json)"
    echo "3) Scenario 3: Enterprise Deployment Gateway Graph (03_enterprise_deploy.json)"
    echo "4) Scenario 4: Multi-Step JSONata Transformations (04_multistep_jsonata.json)"
    echo "5) Scenario 5: Concurrency and Early Resuming Join (05_concurrency_resume.json)"
    echo "6) Scenario 6: Secrets Redaction Demo (06_secrets_redaction.json)"
    echo "7) Scenario 7: Conditional Execution (07_conditionals.json)"
    echo "8) Scenario 8: Loop Iterations (08_loops.json)"
    echo "9) Stop/Kill Mock HTTP Server"
    echo "10) Quit"
    echo "=============================================="
    read -p "Select a scenario to run or action (1-10): " choice
    echo ""

    case $choice in
        1)
            echo "Running: cat examples/01_basic_http.json | ./bin/main user_id=admin_user tier=enterprise"
            echo "----------------------------------------------"
            cat examples/01_basic_http.json | ./bin/main user_id=admin_user tier=enterprise
            ;;
        2)
            echo "Building mock plugin if needed..."
            mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin
            echo "Running: ENV_NAME=production-cluster cat examples/02_plugin_pipeline.json | ./bin/main user_id=developer_99"
            echo "----------------------------------------------"
            ENV_NAME=production-cluster cat examples/02_plugin_pipeline.json | ./bin/main user_id=developer_99
            ;;
        3)
            echo "Running: cat examples/03_enterprise_deploy.json | ./bin/main user_id=admin_user tier=enterprise"
            echo "----------------------------------------------"
            cat examples/03_enterprise_deploy.json | ./bin/main user_id=admin_user tier=enterprise
            ;;
        4)
            echo "Running: ENV_NAME=testing-racks cat examples/04_multistep_jsonata.json | ./bin/main user_id=john_doe_pro"
            echo "----------------------------------------------"
            ENV_NAME=testing-racks cat examples/04_multistep_jsonata.json | ./bin/main user_id=john_doe_pro
            ;;
        5)
            echo "Running: cat examples/05_concurrency_resume.json | ./bin/main"
            echo "----------------------------------------------"
            cat examples/05_concurrency_resume.json | ./bin/main
            ;;
        6)
            echo "Running: cat examples/06_secrets_redaction.json | NESTOR_SECRET_API_KEY=super_secret_token_123 NESTOR_SECRET_DB_PASSWORD=my_private_passphrase ./bin/main"
            echo "----------------------------------------------"
            cat examples/06_secrets_redaction.json | NESTOR_SECRET_API_KEY=super_secret_token_123 NESTOR_SECRET_DB_PASSWORD=my_private_passphrase ./bin/main
            ;;
        7)
            echo "Running Scenario 7: Conditionals (with tier=enterprise region=eu)"
            echo "Running: cat examples/07_conditionals.json | ./bin/main user_id=admin_user tier=enterprise region=eu"
            echo "----------------------------------------------"
            cat examples/07_conditionals.json | ./bin/main user_id=admin_user tier=enterprise region=eu
            ;;
        8)
            echo "Running Scenario 8: Loops (while & for_each)"
            echo "Running: cat examples/08_loops.json | ./bin/main"
            echo "----------------------------------------------"
            cat examples/08_loops.json | ./bin/main
            ;;
        9)
            echo "Stopping mock HTTP server..."
            stop_mock_server
            ;;
        10)
            echo "Exiting Scenario Runner."
            nc -z 127.0.0.1 8080 >/dev/null 2>&1
            if [ $? -eq 0 ]; then
                read -p "Would you like to stop the mock HTTP server before quitting? [y/N]: " kill_choice
                if [[ "$kill_choice" =~ ^[Yy]$ ]]; then
                    stop_mock_server
                fi
            fi
            exit 0
            ;;
        *)
            echo "Invalid selection."
            ;;
    esac
    echo ""
done
