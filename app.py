"""
================================================================
 Accident Detection System — Flask API Server
 GCTU Final Year Project
 Uses: Firebase Realtime DB, OSRM (free routing), Nominatim (free geocoding)
================================================================
"""

from flask import Flask, request, jsonify
from flask_cors import CORS
import requests
import firebase_admin
from firebase_admin import credentials, db
from datetime import datetime
import json

app = Flask(__name__)
CORS(app)

# ─── FIREBASE SETUP ──────────────────────────────────────────────
import os
import json

# Load Firebase credentials from environment variable or local file
firebase_creds = os.environ.get('FIREBASE_CREDENTIALS')
if firebase_creds:
    cred = credentials.Certificate(json.loads(firebase_creds))
else:
    cred = credentials.Certificate("serviceAccountKey.json")

firebase_admin.initialize_app(cred, {
    'databaseURL': 'https://accident-detection-syste-9eb45-default-rtdb.firebaseio.com'
})
# ─── HELPER: Find nearest hospital using Nominatim (FREE) ────────
def find_nearest_hospital(lat, lng):
    try:
        url = "https://nominatim.openstreetmap.org/search"
        params = {
            "q": "hospital",
            "format": "json",
            "limit": 1,
            "viewbox": f"{lng-0.1},{lat+0.1},{lng+0.1},{lat-0.1}",
            "bounded": 1
        }
        headers = {"User-Agent": "SangeCarsEMS/1.0"}
        response = requests.get(url, params=params, headers=headers, timeout=10)
        data = response.json()
        if data:
            hospital = data[0]
            return {
                "name": hospital.get("display_name", "Nearest Hospital").split(",")[0],
                "lat": float(hospital["lat"]),
                "lng": float(hospital["lon"])
            }
    except Exception as e:
        print(f"[Nominatim Error] {e}")
    return {"name": "Hospital (GPS unavailable)", "lat": lat, "lng": lng}

# ─── HELPER: Get fastest route using OSRM (FREE) ─────────────────
def get_fastest_route(from_lat, from_lng, to_lat, to_lng):
    try:
        url = f"http://router.project-osrm.org/route/v1/driving/{from_lng},{from_lat};{to_lng},{to_lat}"
        params = {"overview": "full", "geometries": "geojson", "steps": "false"}
        response = requests.get(url, params=params, timeout=10)
        data = response.json()
        if data.get("code") == "Ok":
            route = data["routes"][0]
            distance_km = round(route["distance"] / 1000, 2)
            duration_min = round(route["duration"] / 60, 1)
            coordinates = route["geometry"]["coordinates"]
            return {
                "distance_km": distance_km,
                "duration_min": duration_min,
                "coordinates": coordinates
            }
    except Exception as e:
        print(f"[OSRM Error] {e}")
    return {"distance_km": 0, "duration_min": 0, "coordinates": []}

# ─── MAIN ENDPOINT: Receive accident report ──────────────────────
@app.route('/api/accident', methods=['POST'])
def receive_accident():
    try:
        data = request.get_json()

        if not data:
            return jsonify({"status": "error", "message": "No data received"}), 400

        # Extract fields from Arduino POST request
        vehicle  = data.get("vehicle",  "Unknown Vehicle")
        plate    = data.get("plate",    "Unknown Plate")
        lat      = float(data.get("lat",  0))
        lng      = float(data.get("lng",  0))
        gforce   = float(data.get("gforce", 0))
        acc_time = data.get("time",     datetime.utcnow().strftime("%H:%M:%S UTC"))
        acc_date = data.get("date",     datetime.utcnow().strftime("%d/%m/%Y"))

        print(f"\n[ACCIDENT] {vehicle} | {plate} | G={gforce} | {lat},{lng}")

        # Find nearest hospital
        hospital = find_nearest_hospital(lat, lng)
        print(f"[HOSPITAL] {hospital['name']}")

        # Get fastest route from hospital to accident
        route = get_fastest_route(
            hospital["lat"], hospital["lng"],
            lat, lng
        )
        print(f"[ROUTE] {route['distance_km']}km | {route['duration_min']}min")

        # Build accident record
        accident_record = {
            "vehicle":       vehicle,
            "plate":         plate,
            "lat":           lat,
            "lng":           lng,
            "gforce":        gforce,
            "time":          acc_time,
            "date":          acc_date,
            "timestamp":     datetime.utcnow().isoformat(),
            "status":        "ACTIVE",
            "hospital":      hospital,
            "route":         route,
            "maps_link":     f"https://maps.google.com/?q={lat},{lng}"
        }

        # Push to Firebase Realtime Database
        ref = db.reference("/accidents")
        new_ref = ref.push(accident_record)
        print(f"[FIREBASE] Saved with key: {new_ref.key}")

        return jsonify({
            "status":    "success",
            "message":   "Accident recorded",
            "key":       new_ref.key,
            "hospital":  hospital["name"],
            "route_km":  route["distance_km"],
            "eta_min":   route["duration_min"]
        }), 200

    except Exception as e:
        print(f"[ERROR] {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

# ─── GET all accidents ────────────────────────────────────────────
@app.route('/api/accidents', methods=['GET'])
def get_accidents():
    try:
        ref = db.reference("/accidents")
        accidents = ref.get()
        return jsonify(accidents or {}), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500

# ─── UPDATE accident status ───────────────────────────────────────
@app.route('/api/accident/<key>/resolve', methods=['POST'])
def resolve_accident(key):
    try:
        ref = db.reference(f"/accidents/{key}")
        ref.update({"status": "RESOLVED"})
        return jsonify({"status": "success", "message": "Accident resolved"}), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500

# ─── HEALTH CHECK ─────────────────────────────────────────────────
@app.route('/', methods=['GET'])
def health():
    return jsonify({
        "status":  "running",
        "project": "Sange Cars EMS",
        "version": "1.0"
    }), 200

# ─── SERVE DASHBOARD ─────────────────────────────────────────────
from flask import render_template

@app.route('/dashboard')
def dashboard():
    return render_template('dashboard.html')

# ─────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    print("=" * 50)
    print(" Sange Cars EMS — Flask API Server")
    print(" Running on http://localhost:5000")
    print("=" * 50)
    app.run(debug=True, host='0.0.0.0', port=5000)
