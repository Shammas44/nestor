from fastapi import FastAPI, Request
from pydantic import BaseModel
import uvicorn

app = FastAPI(title="Nestor Mock HTTP Server")

# Global counter to track provisioning poll state across requests
poll_count = 0

class ProvisionRequest(BaseModel):
    type: str = None
    owner: str = None
    service: str = None
    tier: str = None

class ValidateRequest(BaseModel):
    tier: str = "standard"

@app.post("/api/provision")
@app.post("/provision")
async def provision_resource():
    return {
        "resource_id": "res_12345",
        "status": "ACCEPTED"
    }

@app.post("/api/validate")
@app.post("/validate")
async def validate_payload(req: ValidateRequest):
    return {
        "valid": True,
        "tier": req.tier
    }

@app.get("/api/status/{resource_id}")
@app.get("/status/{resource_id}")
async def get_status(resource_id: str):
    global poll_count
    poll_count += 1
    status = "PENDING"
    if poll_count >= 3:
        status = "ACTIVE"
    return {
        "resource_id": resource_id,
        "status": status,
        "poll_count": poll_count
    }

@app.api_route("/delay/{seconds}", methods=["GET", "POST"])
async def delay_endpoint(seconds: float):
    import asyncio
    await asyncio.sleep(seconds)
    return {
        "status": "completed",
        "delay": seconds
    }

@app.post("/api/deploy/secret")
async def deploy_secret_endpoint():
    return {
        "status": "success",
        "api_key": "super_secret_token_123"
    }

# Catch-all endpoint for general echos/diagnostics
@app.api_route("/{path_name:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH"])
async def catch_all(request: Request, path_name: str):
    if path_name == "error" or path_name == "api/error":
        from fastapi import Response
        return Response(content="Internal Server Error", status_code=500)
    body = None
    raw_body = b""
    try:
        raw_body = await request.body()
        print(f"[SERVER DEBUG] path: /{path_name}, raw_body: '{raw_body.decode()}'", flush=True)
    except Exception as e:
        print(f"[SERVER DEBUG] Error reading body: {e}", flush=True)

    try:
        import json
        if raw_body:
            body = json.loads(raw_body.decode('utf-8'))
    except Exception:
        pass
    return {
        "path": f"/{path_name}",
        "method": request.method,
        "received_body": body
    }

if __name__ == '__main__':
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    uvicorn.run("main:app", host="127.0.0.1", port=port, reload=False)
