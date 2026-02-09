from flask import Flask, request, jsonify, render_template, Response
from flask_cors import CORS
from ultralytics import YOLO
import cv2
import numpy as np
import os
from datetime import datetime
from backend_integration import BackendIntegration

app = Flask(__name__)
CORS(app)  # Enable CORS for web interface

# Get absolute path to model
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_PATH = os.path.join(BASE_DIR, "models/best.pt")

# Load model YOLO đã train
model = YOLO(MODEL_PATH)


# Tạo folder lưu ảnh nếu chưa có
CAPTURES_DIR = os.path.join(BASE_DIR, "captures")
os.makedirs(CAPTURES_DIR, exist_ok=True)
AMBIGUOUS_DIR = os.path.join(CAPTURES_DIR, "ambiguous")
os.makedirs(AMBIGUOUS_DIR, exist_ok=True)

BACKEND_URL = "https://vending-machine-api-1-5.onrender.com"

BACKEND_API_KEY = None  # API key nếu có, để None nếu không cần


@app.route("/detect", methods=["POST"])
def detect_money():
    try:
        # Nhận dữ liệu ảnh từ ESP32-CAM
        file_bytes = np.frombuffer(request.data, np.uint8)
        img = cv2.imdecode(file_bytes, cv2.IMREAD_COLOR)

        if img is None:
            return jsonify({"status": "ERROR", "error": "Cannot decode image"}), 400

        # Kiểm tra xem có yêu cầu lưu ảnh không
        save_image = request.headers.get('X-Save-Image', 'false').lower() == 'true'

        # Chạy nhận dạng
        results = model(img)
        names = model.names

        boxes = results[0].boxes
        if len(boxes) == 0:
            if not save_image:
                print("❌ Không phát hiện tiền.")
            return jsonify({"status": "NO_DETECTION", "message": "Không phát hiện tiền"})

        # Lấy top1 và top2
        best_box = boxes[0]
        cls_id = int(best_box.cls[0])
        conf = float(best_box.conf[0])
        label = names[cls_id]
        top2_conf = float(boxes[1].conf[0]) if len(boxes) > 1 else 0.0

        # Lấy tọa độ bounding box (x1, y1, x2, y2)
        x1, y1, x2, y2 = best_box.xyxy[0].cpu().numpy()
        bbox = {"x1": int(x1), "y1": int(y1), "x2": int(x2), "y2": int(y2)}

        # Ngưỡng và margin
        CONFIDENCE_THRESHOLD = 0.4
        CONFIDENCE_MARGIN = 0.03

        # Nếu ambiguous -> trả LOW_CONFIDENCE và lưu ảnh
        if conf < CONFIDENCE_THRESHOLD or (conf - top2_conf) < CONFIDENCE_MARGIN:
            print(f"⚠️ Ambiguous detection: top1={conf:.2%}, top2={top2_conf:.2%}")
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            amb_fn = os.path.join(AMBIGUOUS_DIR, f"amb_{timestamp}_{label}_{int(conf*1000)}.jpg")
            try:
                cv2.imwrite(amb_fn, img)
            except Exception as e:
                print(f"⚠️ Failed to save ambiguous image: {e}")
            return jsonify({
                "status": "LOW_CONFIDENCE",
                "denomination": label,
                "confidence": round(conf, 4),
                "top2_confidence": round(top2_conf, 4),
                "message": "Độ tin cậy thấp hoặc không đủ khác biệt giữa 2 nhãn hàng đầu"
            })

        # SUCCESS path
        print(f"✅ OK - Detected: {label} VND - Confidence: {conf:.2%}")
        
        # Lưu ảnh GỐC (không bounding box) để gửi backend
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        original_filename = os.path.join(CAPTURES_DIR, f"{label}_{timestamp}_original.jpg")
        cv2.imwrite(original_filename, img)
        
        # Lưu ảnh CÓ bounding box để preview
        img_with_box = img.copy()
        cv2.rectangle(img_with_box, (int(x1), int(y1)), (int(x2), int(y2)), (0, 255, 0), 2)
        text = f"{label} VND ({conf:.2%})"
        cv2.putText(img_with_box, text, (int(x1), int(y1)-10), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)
        
        preview_filename = os.path.join(CAPTURES_DIR, f"{label}_{timestamp}_preview.jpg")
        cv2.imwrite(preview_filename, img_with_box)
        print(f"✅ Nhận diện thành công! Đã lưu: {original_filename}")

        response_data = {
            "status": "SUCCESS",
            "denomination": label,
            "confidence": round(conf, 4),
            "bbox": bbox,
            "saved_image": preview_filename,
            "original_image": original_filename,
            "message": f"Nhận diện {label} VND",
            "timestamp": timestamp
        }

        # 🆕 GỬI ẢNH GỐC + DATA ĐẾN BACKEND
        # 🆕 GỬI ẢNH GỐC + DATA ĐẾN BACKEND (an toàn với label không phải số)
        if backend:
            if label.isdigit():  # Chỉ gửi khi label là số
                amount = int(label)  # VD: "10000" -> 10000
                confident = round(conf, 2)  # VD: 0.9872 -> 0.98
                try:
                    backend_response = backend.send_money_upload(original_filename, amount, confident)
                    if backend_response:
                        response_data["backend_status"] = "SUCCESS"
                        response_data["backend_response"] = backend_response
                    else:
                        response_data["backend_status"] = "FAILED"
                except Exception as e:
                    print(f"⚠️ Backend upload failed: {e}")
                    response_data["backend_status"] = "FAILED"
            else:
                print(f"⚠️ Label không phải số: {label}, bỏ gửi backend")
                response_data["backend_status"] = "SKIPPED"

        return jsonify(response_data)
    except Exception as e:
        print(f"❌ Error: {e}")
        return jsonify({"status": "ERROR", "error": str(e)}), 500


last_denomination = ""
last_timestamp = ""
@app.after_request
def store_last_response(response):
    global last_denomination,last_timestamp
    try:
        data = response.get_json(silent=True)
        # Chỉ ghi khi là phản hồi SUCCESS (từ /detect)
        if data and data.get("status") == "SUCCESS":
            last_denomination = str(data.get("denomination") or "")
            last_timestamp = str(data.get("timestamp") or "")
    except Exception as e:
        # Không làm server lỗi vì bước lưu bộ nhớ tạm
        print(f"⚠️ after_request store error: {e}")
    return response

@app.route("/latest", methods=["GET"])
def latest():
    if last_denomination:
        # FIX: đảm bảo timestamp là chuỗi, không phải None
        return jsonify({
            "status": "OK",
            "denomination": last_denomination,
            "timestamp": last_timestamp or ""
        })
    else:
        return jsonify({"status": "NO_DATA"})
# Khởi tạo Backend Integration
backend = None
if BACKEND_URL:
    backend = BackendIntegration(BACKEND_URL, BACKEND_API_KEY)
    print(f"🔗 Backend Integration: {BACKEND_URL}")

if __name__ == "__main__":
    print("🚀 Server nhận diện tiền đang khởi động...")
    print(f"📦 Model: {MODEL_PATH}")
    print(f"📁 Ảnh sẽ được lưu tại: {CAPTURES_DIR}")
    print(f"🌐 Server: http://0.0.0.0:5001")
    app.run(host="0.0.0.0", port=5001, debug=False)
