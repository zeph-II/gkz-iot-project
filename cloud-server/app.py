import os
import sqlite3
from datetime import datetime, timezone
from flask import Flask, request, jsonify, Response
from flask_cors import CORS

app = Flask(__name__)
CORS(app)  # lets the GitHub Pages dashboards call this from a different origin

DB_PATH = os.environ.get("DB_PATH", "readings.db")
# Set this in Render's Environment Variables tab - must match the value
# flashed into the ESP32 firmware and used by the dashboards.
API_KEY = os.environ.get("API_KEY", "changeme")


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts TEXT,
            tempC REAL,
            humidity REAL,
            pressureHpa REAL,
            coPpm REAL,
            voltageV REAL,
            vibrationG REAL,
            uptimeS INTEGER,
            received_at TEXT
        )
        """
    )
    conn.commit()
    conn.close()


init_db()

# Most recent reading, kept in memory so /api/sensors is instant and
# doesn't need a DB round-trip for the common "give me live data" case.
latest = {}


def check_key():
    header_key = request.headers.get("X-API-Key", "")
    query_key = request.args.get("key", "")
    return API_KEY in (header_key, query_key)


@app.route("/api/ingest", methods=["POST"])
def ingest():
    """The ESP32 calls this every ~60s to push a new reading. Requires
    the API key since it's the one endpoint that writes data."""
    if not check_key():
        return jsonify({"error": "unauthorized"}), 401
    data = request.get_json(force=True, silent=True) or {}
    global latest
    latest = data
    conn = get_db()
    conn.execute(
        "INSERT INTO readings (ts, tempC, humidity, pressureHpa, coPpm, voltageV, vibrationG, uptimeS, received_at) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        (
            data.get("ts", ""),
            data.get("tempC"),
            data.get("humidity"),
            data.get("pressureHpa"),
            data.get("coPpm"),
            data.get("voltageV"),
            data.get("vibrationG"),
            data.get("uptimeS"),
            datetime.now(timezone.utc).isoformat(),
        ),
    )
    conn.commit()
    conn.close()
    return jsonify({"ok": True})


@app.route("/api/sensors", methods=["GET"])
def sensors():
    if not check_key():
        return jsonify({"error": "unauthorized"}), 401
    return jsonify(
        {
            "env": {
                "available": "tempC" in latest,
                "tempC": latest.get("tempC"),
                "humidity": latest.get("humidity"),
                "pressureHpa": latest.get("pressureHpa"),
            },
            "gas": {"coPpmEstimate": latest.get("coPpm"), "voltageV": latest.get("voltageV")},
            "imu": {"available": "vibrationG" in latest, "vibrationG": latest.get("vibrationG")},
            "device": {"uptimeS": latest.get("uptimeS")},
            "time": {"available": bool(latest.get("ts")), "iso": latest.get("ts", "")},
        }
    )


@app.route("/api/history", methods=["GET"])
def history():
    if not check_key():
        return jsonify({"error": "unauthorized"}), 401
    limit = request.args.get("limit", type=int)
    conn = get_db()
    if limit:
        rows = conn.execute("SELECT * FROM readings ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        rows = list(reversed(rows))
    else:
        rows = conn.execute("SELECT * FROM readings ORDER BY id ASC").fetchall()
    conn.close()
    result = [
        {
            "ts": r["ts"],
            "tempC": r["tempC"],
            "humidity": r["humidity"],
            "pressureHpa": r["pressureHpa"],
            "coPpm": r["coPpm"],
            "voltageV": r["voltageV"],
            "vibrationG": r["vibrationG"],
            "uptimeS": r["uptimeS"],
        }
        for r in rows
    ]
    return jsonify({"count": len(result), "rows": result})


@app.route("/api/history/csv", methods=["GET"])
def history_csv():
    if not check_key():
        return jsonify({"error": "unauthorized"}), 401
    conn = get_db()
    rows = conn.execute("SELECT * FROM readings ORDER BY id ASC").fetchall()
    conn.close()
    lines = ["ts,tempC,humidity,pressureHpa,coPpm,voltageV,vibrationG,uptimeS"]
    for r in rows:
        lines.append(
            f'{r["ts"]},{r["tempC"]},{r["humidity"]},{r["pressureHpa"]},{r["coPpm"]},{r["voltageV"]},{r["vibrationG"]},{r["uptimeS"]}'
        )
    return Response(
        "\n".join(lines),
        mimetype="text/csv",
        headers={"Content-Disposition": "attachment; filename=history.csv"},
    )


@app.route("/api/history/clear", methods=["POST"])
def history_clear():
    if not check_key():
        return jsonify({"error": "unauthorized"}), 401
    conn = get_db()
    conn.execute("DELETE FROM readings")
    conn.commit()
    conn.close()
    global latest
    latest = {}
    return jsonify({"ok": True})


@app.route("/", methods=["GET"])
def index():
    return jsonify({"status": "ok", "service": "IoT sensor cloud relay"})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 5000)))
