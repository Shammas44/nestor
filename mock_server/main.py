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

# Multi-region authentication & Employee directory endpoints
EMPLOYEE_DB = {
    "us-east": [
        {"id": "emp_101", "name": "Alice Smith", "age": 25, "department": "Engineering", "salary": 95000, "region": "us-east", "internal_notes": "Top performer"},
        {"id": "emp_102", "name": "Bob Jones", "age": 28, "department": "Product", "salary": 88000, "region": "us-east", "internal_notes": "Remote"},
        {"id": "emp_103", "name": "Charlie Brown", "age": 42, "department": "Executive", "salary": 170000, "region": "us-east", "internal_notes": "HQ"}
    ],
    "us-west": [
        {"id": "emp_201", "name": "David Miller", "age": 22, "department": "Design", "salary": 78000, "region": "us-west", "internal_notes": "Intern"},
        {"id": "emp_202", "name": "Eve Davis", "age": 35, "department": "Engineering", "salary": 120000, "region": "us-west", "internal_notes": "Lead"}
    ],
    "eu-west": [
        {"id": "emp_301", "name": "Fiona Garcia", "age": 29, "department": "Operations", "salary": 82000, "region": "eu-west", "internal_notes": "Paris"},
        {"id": "emp_302", "name": "George Wilson", "age": 31, "department": "Sales", "salary": 91000, "region": "eu-west", "internal_notes": "London"}
    ],
    "ap-south": [
        {"id": "emp_401", "name": "Hana Tanaka", "age": 26, "department": "Engineering", "salary": 85000, "region": "ap-south", "internal_notes": "Tokyo"},
        {"id": "emp_402", "name": "Ian Chen", "age": 24, "department": "Marketing", "salary": 72000, "region": "ap-south", "internal_notes": "Singapore"}
    ]
}

@app.post("/api/v1/{region_id}/auth/login")
async def region_login(region_id: str, request: Request):
    from fastapi import Response
    token = f"Bearer token_{region_id}_secret"
    resp = Response(
        content=f'{{"status": "authenticated", "region": "{region_id}", "token": "{token}"}}',
        media_type="application/json"
    )
    resp.headers["Authorization"] = token
    return resp

@app.get("/api/v1/{region_id}/employees")
async def get_region_employees(region_id: str, max_age: int = 100):
    emps = EMPLOYEE_DB.get(region_id, [])
    ids = [e["id"] for e in emps if e["age"] < max_age]
    return {
        "region": region_id,
        "employee_ids": ids
    }

@app.get("/api/v1/{region_id}/employees/{emp_id}")
async def get_employee_details(region_id: str, emp_id: str):
    emps = EMPLOYEE_DB.get(region_id, [])
    for e in emps:
        if e["id"] == emp_id:
            return e
    from fastapi import HTTPException
    raise HTTPException(status_code=404, detail="Employee not found")

@app.get("/api/bigdata/{size_mb}")
async def get_big_data(size_mb: float, nodata: bool = False):
    num_elements = int(size_mb * 1024)
    if nodata:
        return {
            "size_mb": size_mb,
            "element_count": num_elements
        }
    chunk = "A" * 1024
    data = [{"id": i, "payload": chunk} for i in range(num_elements)]
    return {
        "size_mb": size_mb,
        "element_count": num_elements,
        "data": data
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
