# IoT Sensor Cloud Relay

A tiny Flask server that sits between your ESP32 sensor node and your
dashboards. The ESP32 pushes readings to this server (outbound POST,
no port forwarding needed on your home network); the dashboards read
from it (GET). It has its own SQLite database and, once deployed,
its own public HTTPS URL - no router configuration required.

## Deploying (Render.com, free tier)

1. Push this `cloud-server` folder to a GitHub repo (can be a new
   repo, or a subfolder of your existing one).
2. On render.com, New > Web Service > connect that repo.
   - Root directory: `cloud-server` (if it's a subfolder)
   - Build command: `pip install -r requirements.txt`
   - Start command: `gunicorn app:app`
3. Under Environment, add a variable `API_KEY` set to a long random
   string - this is the shared secret the ESP32 and dashboards must
   also use. Generate one with, e.g., a password manager.
4. Deploy. Render gives you a URL like
   `https://your-service.onrender.com`.
5. (Optional) Settings > Custom Domain > add your own domain, then
   add the CNAME record Render shows you at your DNS provider.

## Notes

- Free-tier Render services spin down after inactivity and take
  ~30-50s to wake on the next request - fine for a personal
  dashboard, just expect an occasional slow first load.
- The SQLite file is NOT persistent on Render's free tier (resets on
  redeploy/restart). This is fine here because the ESP32 keeps its
  own copy of the log on its flash as the durable backup - this
  server is a live relay, not the source of truth.
