# ── Stage 1: Build React frontend ──────────────────────────────────────────
FROM node:20-alpine AS frontend-build
WORKDIR /frontend
COPY frontend/package.json ./
RUN npm install
COPY frontend/ ./
RUN npm run build

# ── Stage 2: Build Go backend ───────────────────────────────────────────────
FROM golang:1.25-alpine AS backend-build
WORKDIR /backend
COPY backend/ ./
RUN go mod tidy && CGO_ENABLED=0 GOOS=linux go build -o server .

# ── Stage 3: Final minimal image ────────────────────────────────────────────
FROM alpine:3.19
WORKDIR /app
COPY --from=backend-build  /backend/server   ./
COPY --from=frontend-build /frontend/dist    ./static

# /data is a persistent volume for the SQLite database
RUN mkdir -p /data
VOLUME ["/data"]

ENV PORT=8080 \
    STATIC_DIR=/app/static \
    MQTT_BROKER=mosquitto \
    MQTT_PORT=1883 \
    MQTT_USER=tankmonitor \
    MQTT_PASS=###TankMonitor12345 \
    AUTH_USER=admin \
    AUTH_PASS=tank1234 \
    AUTH_SECRET= \
    DB_PATH=/data/tankmonitor.db

EXPOSE 8080
CMD ["./server"]
