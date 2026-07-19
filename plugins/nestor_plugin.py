import os
import sys
import json

class NestorPlugin:
    """
    Nestor Custom Plugin SDK for Python.
    Provides helpers to read execution context and output results.
    """

    @staticmethod
    def get_input() -> dict:
        """
        Retrieves the plugin specific input variables passed by the engine.
        """
        path = os.getenv("NESTOR_PLUGIN_INPUT")
        if not path or not os.path.exists(path):
            return {}
        try:
            with open(path, "r") as f:
                return json.load(f)
        except Exception:
            return {}

    @staticmethod
    def get_env() -> dict:
        """
        Retrieves the host environment variables context passed by the engine.
        """
        env_str = os.getenv("NESTOR_ENV")
        if not env_str:
            return {}
        try:
            return json.loads(env_str)
        except Exception:
            return {}

    @staticmethod
    def success(body: dict):
        """
        Emits a structured JSON payload to stdout and exits cleanly.
        """
        print(json.dumps(body))
        sys.exit(0)

    @staticmethod
    def error(exit_code: int, message: str):
        """
        Prints an error message to stderr and exits with a non-zero code.
        """
        print(f"Error: {message}", file=sys.stderr)
        sys.exit(exit_code if exit_code != 0 else 1)
