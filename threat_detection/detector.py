from ultralytics import YOLO
import cv2, requests, numpy as np, time, os, json

# -------- SETTINGS --------
MODEL_PATH = os.environ.get("BEE_MODEL_PATH", os.path.join("weights", "best.pt"))
CAMERA_URL = os.environ.get("BEE_CAMERA_URL", "http://ESP32_CAM_IP:81/stream")  # IP printed in the ESP32-CAM serial monitor
CONFIDENCE, IOU, IMAGE_SIZE, MAX_DETECTIONS = 0.25, 0.45, 640, 50
ENTRANCE_X1, ENTRANCE_Y1, ENTRANCE_X2, ENTRANCE_Y2 = 208, 198, 502, 277
BOX_PADDING_RATIO = 0.30
HONEYBEE_CLASSES = {"honeybee", "honey bee", "bee"}
THREAT_CLASSES = {"ant", "hornet", "wasp", "wasps", "wax moth", "wax_moth"}
THREAT_FRAME_THRESHOLD, THREAT_DURATION_SECONDS = 1, 3.0
SAFE_PUBLISH_INTERVAL = 60.0  # seconds
THREAT_PUBLISH_INTERVAL = 3.0   # Repeat every 3 seconds while threat persists
STATUS_FEED = "bee-hive-detection"

# Credentials come from environment variables (never hardcode them):
#   Windows: setx AIO_USERNAME "your_user"  /  setx AIO_KEY "your_key"
AIO_USERNAME = os.environ.get("AIO_USERNAME", "")
AIO_KEY = os.environ.get("AIO_KEY", "")
AIO_HOST, AIO_PORT = "io.adafruit.com", 1883

mqtt_client = None
mqtt_connected = False
mqtt_available = False
mqtt_last_error_time = 0.0


def setup_mqtt():
    global mqtt_client, mqtt_available
    if not AIO_USERNAME or not AIO_KEY:
        print("[MQTT ERROR] Set the AIO_USERNAME and AIO_KEY environment variables.")
        print("[MQTT] Detection will run locally, but data will NOT be published until the key is set.")
        return
    try:
        import paho.mqtt.client as mqtt
        try:
            client = mqtt.Client(
                mqtt.CallbackAPIVersion.VERSION2,
                client_id=f"bee-hive-{int(time.time())}"
            )
        except (AttributeError, TypeError):
            client = mqtt.Client(client_id=f"bee-hive-{int(time.time())}")
        client.username_pw_set(AIO_USERNAME, AIO_KEY)

        def on_connect(client, userdata, flags, reason_code, properties=None):
            global mqtt_connected
            failed = getattr(reason_code, "is_failure", None)
            if failed is None:
                try:
                    failed = int(reason_code) != 0
                except (TypeError, ValueError):
                    failed = str(reason_code).lower() not in ("success", "0")
            mqtt_connected = not failed
            print("[MQTT] Connected to Adafruit IO." if mqtt_connected
                  else f"[MQTT ERROR] Broker refused connection: {reason_code}")

        def on_disconnect(client, userdata, *args):
            global mqtt_connected
            mqtt_connected = False
            print("[MQTT] Disconnected from Adafruit IO.")

        client.on_connect = on_connect
        client.on_disconnect = on_disconnect
        mqtt_client = client
        client.connect(AIO_HOST, AIO_PORT, keepalive=60)
        client.loop_start()
        mqtt_available = True
        print("[MQTT] Connection requested; waiting for broker confirmation.")
    except ImportError:
        print("[MQTT ERROR] paho-mqtt missing. Run: python -m pip install paho-mqtt")
    except Exception as exc:
        print(f"[MQTT ERROR] Could not connect: {exc}")
        print("[MQTT] Check internet, username, NEW key, and port 1883.")


def publish_feed(feed_name, value):
    global mqtt_last_error_time
    if mqtt_client is None or not mqtt_available:
        return False
    if not mqtt_connected:
        now = time.time()
        if now - mqtt_last_error_time >= 10:
            print("[MQTT] Not connected; skipping publish for now.")
            mqtt_last_error_time = now
        return False
    topic = f"{AIO_USERNAME}/feeds/{feed_name}"
    try:
        result = mqtt_client.publish(topic, str(value), qos=0)
        if result.rc != 0:
            print(f"[MQTT ERROR] {feed_name} publish return code: {result.rc}")
            return False
        print(f"[MQTT] Publish queued: {feed_name} = {value}")
        return True
    except Exception as exc:
        print(f"[MQTT ERROR] Publishing {feed_name}: {exc}")
        return False


def connect_camera():
    while True:
        try:
            print(f"[CAMERA] Connecting to {CAMERA_URL}")
            response = requests.get(CAMERA_URL, stream=True, timeout=(5, 45))
            response.raise_for_status()
            print("[CAMERA] Connected.")
            return response
        except Exception as exc:
            print(f"[CAMERA ERROR] {exc}; retrying in 2 seconds.")
            time.sleep(2)


def padded_box(x1, y1, x2, y2, width, height):
    px, py = int(max(0, x2-x1)*BOX_PADDING_RATIO), int(max(0, y2-y1)*BOX_PADDING_RATIO)
    return max(0, x1-px), max(0, y1-py), min(width-1, x2+px), min(height-1, y2+py)


def norm_name(name):
    return str(name).lower().strip().replace("-", " ").replace("_", " ")


def main():
    global mqtt_connected
    print("=" * 58, "\nBEE HIVE MONITORING - YOLO + COUNTING + MQTT\n", "=" * 58)
    if not os.path.isfile(MODEL_PATH):
        print(f"[ERROR] Model not found: {MODEL_PATH}")
        print("Place best.pt in threat_detection/weights/ or set BEE_MODEL_PATH.")
        input("Press Enter to close...")
        return

    print("[YOLO] Loading model...")
    model = YOLO(MODEL_PATH)
    print("[YOLO] Classes:", model.names)
    print("[CAMERA] URL:", CAMERA_URL)
    print("[INFO] ESP32-CAM is used only as the video stream source.")
    print("[INFO] Python/YOLO publishes SAFE or THREAT DETECTED to one feed: bee-hive-detection.")
    print("[INFO] Threat notification starts after 3 continuous seconds, then repeats every 3 seconds while threat persists.")
    print("[INFO] SAFE status publishes at most once every 10 minutes when bee movement is detected and no threat is present.")
    setup_mqtt()

    entry_count = exit_count = 0
    previous_zone_by_id = {}
    previous_centers_by_id = {}
    previous_threat_in_frame = False
    threat_frames, threat_start_time = 0, None
    alert_sent_for_episode = False
    last_safe_publish = 0.0
    last_threat_publish = 0.0
    previous_frame_time, fps = time.time(), 0.0

    try:
        while True:
            response = connect_camera()
            buffer = b""
            try:
                for chunk in response.iter_content(chunk_size=4096):
                    if not chunk:
                        continue
                    buffer += chunk
                    while True:
                        start = buffer.find(b"\xff\xd8")
                        if start < 0:
                            buffer = buffer[-1:]
                            break
                        end = buffer.find(b"\xff\xd9", start+2)
                        if end < 0:
                            if start > 0:
                                buffer = buffer[start:]
                            break
                        jpg, buffer = buffer[start:end+2], buffer[end+2:]
                        frame = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
                        if frame is None:
                            continue

                        height, width = frame.shape[:2]
                        sx, sy = width/640.0, height/480.0
                        bx1, by1 = int(ENTRANCE_X1*sx), int(ENTRANCE_Y1*sy)
                        bx2, by2 = int(ENTRANCE_X2*sx), int(ENTRANCE_Y2*sy)

                        result = model.track(
                            source=frame, persist=True, tracker="bytetrack.yaml",
                            imgsz=IMAGE_SIZE, conf=CONFIDENCE, iou=IOU,
                            max_det=MAX_DETECTIONS, device="cpu", verbose=False
                        )[0]

                        honeybee_count = threat_count = 0
                        threat_in_frame = False
                        bee_movement_detected = False
                        if result.boxes is not None:
                            for box in result.boxes:
                                class_id = int(box.cls[0])
                                raw_name = str(model.names[class_id])
                                name = norm_name(raw_name)
                                confidence = float(box.conf[0])
                                x1, y1, x2, y2 = map(int, box.xyxy[0])
                                x1, y1 = max(0, x1), max(0, y1)
                                x2, y2 = min(width-1, x2), min(height-1, y2)
                                cx, cy = (x1+x2)//2, (y1+y2)//2
                                track_id = int(box.id[0]) if box.id is not None and len(box.id) else None

                                if name in HONEYBEE_CLASSES:
                                    honeybee_count += 1
                                    label = f"Bee ID:{track_id if track_id is not None else '?'} {confidence:.0%}"
                                    dx1, dy1, dx2, dy2 = padded_box(x1, y1, x2, y2, width, height)
                                    cv2.rectangle(frame, (dx1,dy1), (dx2,dy2), (0,255,0), 2)
                                    if track_id is not None:
                                        # Treat a tracked bee moving at least 5 pixels
                                        # between frames as movement.
                                        previous_center = previous_centers_by_id.get(track_id)
                                        if previous_center is not None:
                                            dx = cx - previous_center[0]
                                            dy = cy - previous_center[1]
                                            if (dx * dx + dy * dy) >= 25:
                                                bee_movement_detected = True
                                        previous_centers_by_id[track_id] = (cx, cy)

                                        inside_now = bx1 <= cx <= bx2 and by1 <= cy <= by2
                                        inside_before = previous_zone_by_id.get(track_id)
                                        if inside_before is not None:
                                            if inside_now and not inside_before:
                                                entry_count += 1
                                                print(f"[COUNT] ENTRY Bee ID {track_id}; total={entry_count}")
                                            elif inside_before and not inside_now:
                                                exit_count += 1
                                                print(f"[COUNT] EXIT Bee ID {track_id}; total={exit_count}")
                                        previous_zone_by_id[track_id] = inside_now
                                elif name in THREAT_CLASSES:
                                    threat_count += 1
                                    threat_in_frame = True
                                    label = f"THREAT: {raw_name} {confidence:.0%}"
                                    cv2.rectangle(frame, (x1,y1), (x2,y2), (0,0,255), 2)
                                else:
                                    label = f"{raw_name} {confidence:.0%}"
                                    cv2.rectangle(frame, (x1,y1), (x2,y2), (255,255,0), 2)

                                cv2.putText(frame, label, (x1,max(y1-8,18)),
                                            cv2.FONT_HERSHEY_SIMPLEX, 0.48, (255,255,255), 2)

                        cv2.rectangle(frame, (bx1,by1), (bx2,by2), (255,255,0), 3)
                        cv2.putText(frame, "HIVE ENTRANCE ZONE", (bx1,max(20,by1-8)),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255,255,0), 2)

                        now = time.time()
                        if threat_in_frame:
                            threat_frames += 1
                            if threat_start_time is None:
                                threat_start_time = now
                                alert_sent_for_episode = False
                        else:
                            threat_frames, threat_start_time = 0, None
                            alert_sent_for_episode = False
                        threat_duration = now-threat_start_time if threat_start_time is not None else 0.0

                        # Start publishing only after 3 continuous seconds of threat.
                        # Repeat the status every 3 seconds while a threat remains present.
                        if (threat_in_frame and
                                threat_duration >= THREAT_DURATION_SECONDS and
                                now - last_threat_publish >= THREAT_PUBLISH_INTERVAL):
                            alert = {
                                "alert": "THREAT DETECTED",
                                "threats_in_current_frame": threat_count,
                                "threat_frames": threat_frames,
                                "duration_seconds": round(threat_duration, 2),
                                "timestamp": int(now)
                            }
                            print("[ALERT] THREAT DETECTED:", json.dumps(alert))
                            if publish_feed(STATUS_FEED, "THREAT DETECTED"):
                                last_threat_publish = now
                            alert_sent_for_episode = True

                        elapsed = now-previous_frame_time
                        if elapsed > 0:
                            instant_fps = 1.0/elapsed
                            fps = instant_fps if fps == 0 else 0.8*fps + 0.2*instant_fps
                        previous_frame_time = now

                        status = "THREAT DETECTED!" if threat_in_frame else "SAFE"
                        color = (0,0,255) if threat_in_frame else (0,255,0)
                        mqtt_text = "CONNECTED" if mqtt_connected else (
                            "CONNECTING" if mqtt_available else "NOT CONFIGURED")
                        lines = [
                            (status,color),
                            (f"HoneyBees in frame: {honeybee_count}",(255,255,255)),
                            (f"ENTRY total: {entry_count}",(255,255,255)),
                            (f"EXIT total: {exit_count}",(255,255,255)),
                            (f"THREATS in frame: {threat_count}",(255,255,255)),
                            (f"FPS: {fps:.1f}",(255,255,255)),
                            (f"Threat frames: {threat_frames}",(255,255,255)),
                            (f"Threat duration: {threat_duration:.1f}s",(255,255,255)),
                            (f"MQTT: {mqtt_text}",(0,255,0) if mqtt_connected else (0,200,255))
                        ]
                        for i, (text, color) in enumerate(lines):
                            cv2.putText(frame, text, (15,30+i*27), cv2.FONT_HERSHEY_SIMPLEX,
                                        0.62 if i == 0 else 0.52, color, 2)

                        # Publish ONLY to the bee-hive-detection feed.
                        # Publish THREAT DETECTED immediately once when a threat appears.
                        # Publish SAFE at most once per minute when tracked bee movement
                        # is observed and no threat is present.
                        if threat_in_frame:
                            # Threat publication is handled above after 3 continuous seconds.
                            previous_threat_in_frame = True
                        else:
                            previous_threat_in_frame = False
                            if (bee_movement_detected and
                                    now - last_safe_publish >= SAFE_PUBLISH_INTERVAL):
                                if publish_feed(STATUS_FEED, "SAFE"):
                                    last_safe_publish = now

                        cv2.imshow("BEE HIVE MONITORING - ENTRANCE ZONE", frame)
                        if cv2.waitKey(1) & 0xFF in (ord("q"), ord("Q")):
                            raise KeyboardInterrupt

            except KeyboardInterrupt:
                raise
            except (requests.exceptions.RequestException, TimeoutError, ConnectionError, OSError) as exc:
                print(f"[CAMERA] Stream interrupted: {exc}; reconnecting in 2 seconds.")
                time.sleep(2)
            finally:
                response.close()
    except KeyboardInterrupt:
        print("\n[INFO] Detection stopped by user.")
    finally:
        cv2.destroyAllWindows()
        if mqtt_client is not None:
            try:
                mqtt_client.loop_stop()
                mqtt_client.disconnect()
            except Exception:
                pass
        print("[INFO] Program finished.")


if __name__ == "__main__":
    main()


