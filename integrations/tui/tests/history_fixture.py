"""Display-record fixtures for the fake daemon, not application code."""


def display_records(messages):
    records, tools = [], {}
    current = previous = None
    for index, message in enumerate(messages):
        role = message["role"]
        if role == "system":
            continue
        identifier = message.get("id", f"fixture-{index}")
        if message.get("compaction") is not None:
            records.append({**message, "id": identifier})
            current = previous = None
            continue
        if role == "user":
            records.append({**message, "id": identifier})
            current = previous = None
        def ensure(part):
            nonlocal current, previous
            if current is None:
                if previous:
                    previous["terminal"] = False
                current = {"id": f"{identifier}/activity/{part}", "role": "activity", "content": [], "kinds": [],
                           "count": 0, "activities": [], "terminal": True}
                records.append(current)
                previous = current
            return current
        if role == "user":
            ensure(0)
            continue
        if message.get("metadata"):
            ensure(0)["metadata"] = message["metadata"]
        for part_index, part in enumerate(message["content"]):
            kind = part["type"]
            if kind == "text" and part["text"].strip():
                records.append({**message, "id": identifier, "content": [part]})
                current = None
            elif kind == "reasoning" and part.get("text"):
                drawer = ensure(part_index)
                drawer["activities"].append({"kind": "reasoning", "text": part["text"]})
                drawer["kinds"].append("reasoning")
                drawer["count"] += 1
            elif kind == "tool_call":
                drawer = ensure(part_index)
                activity = {"kind": "tool", **part}
                drawer["activities"].append(activity)
                drawer["kinds"].append("read" if part["toolName"] == "read" else "tools")
                drawer["count"] += 1
                tools[part["callId"]] = activity
            elif kind == "tool_result" and part["callId"] in tools:
                tools[part["callId"]]["result"] = part
    return records
