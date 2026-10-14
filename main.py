import sqlite3, time, threading, hashlib, unicodedata, io, json, csv, zipfile, base64
import cv2, numpy as np
from fastapi import FastAPI, Request, UploadFile, File, Form, HTTPException, Body
from fastapi.responses import Response
from fastapi.staticfiles import StaticFiles

DB = "locker.db"
THRESH = 0.363  # ngưỡng cosine của SFace; tăng lên (vd 0.45) nếu muốn chặt hơn

det = cv2.FaceDetectorYN.create("models/face_detection_yunet_2023mar.onnx", "", (320, 320), 0.8, 0.3, 5000)
rec = cv2.FaceRecognizerSF.create("models/face_recognition_sface_2021dec.onnx", "")
lock = threading.Lock()
state = {"enroll_uid": None, "until": 0.0, "last_jpg": None}
fp = {"pending": None, "last": None}   # trạng thái đăng ký vân tay
cmds = []                              # lệnh chờ gửi cho ESP32 DevKit (DevKit hỏi mỗi ~0.7s)
lockers = {n: {"door": None, "item": None, "dist": None, "alarm": False} for n in (1, 2)}
dev_seen = {"t": 0.0}
app = FastAPI()


def db():
    c = sqlite3.connect(DB)
    c.row_factory = sqlite3.Row
    c.execute("PRAGMA foreign_keys=ON")
    return c


with db() as c:
    c.executescript("""
    CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, phone TEXT, locker INTEGER);
    CREATE TABLE IF NOT EXISTS faces(id INTEGER PRIMARY KEY, user_id INTEGER REFERENCES users(id) ON DELETE CASCADE, vec BLOB);
    CREATE TABLE IF NOT EXISTS logs(id INTEGER PRIMARY KEY, ts TEXT DEFAULT CURRENT_TIMESTAMP,
                                    user_id INTEGER, score REAL, matched INTEGER, locker INTEGER);
    """)
    # nâng cấp CSDL cũ (không cần xóa locker.db)
    for tbl, col, typ in [("users", "finger_id", "INTEGER"), ("logs", "method", "TEXT"), ("logs", "note", "TEXT"),
                          ("faces", "img", "BLOB"), ("faces", "meta", "TEXT")]:
        if col not in [r[1] for r in c.execute(f"PRAGMA table_info({tbl})")]:
            c.execute(f"ALTER TABLE {tbl} ADD COLUMN {col} {typ}")


def ascii_name(s: str) -> str:  # LCD không hiển thị được dấu tiếng Việt
    s = (s or "").replace("đ", "d").replace("Đ", "D")
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()


def log(uid, score, matched, locker_no, method, note=None):
    with db() as c:
        c.execute("INSERT INTO logs(user_id,score,matched,locker,method,note) VALUES(?,?,?,?,?,?)",
                  (uid, score, matched, locker_no, method, note))


def analyze(jpg: bytes):
    """JPEG -> dict: vec (128 chiều, đã chuẩn hóa), crop (JPEG mặt đã căn chỉnh), bbox, landmarks, det_score.
    Lấy mặt lớn nhất; trả None nếu không thấy mặt."""
    img = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return None
    with lock:
        det.setInputSize((img.shape[1], img.shape[0]))
        _, faces = det.detect(img)
        if faces is None:
            return None
        best = max(faces, key=lambda f: f[2] * f[3])
        crop = rec.alignCrop(img, best)
        f = rec.feature(crop).flatten().astype(np.float32)
    ok, buf = cv2.imencode(".jpg", crop)
    return {
        "vec": f / np.linalg.norm(f),
        "crop": buf.tobytes(),
        "bbox": [round(float(x), 1) for x in best[0:4]],  # x, y, w, h
        "landmarks": [[round(float(best[4 + 2 * i]), 1), round(float(best[5 + 2 * i]), 1)] for i in range(5)],
        "det_score": round(float(best[14]), 4),
        "img_size": [img.shape[1], img.shape[0]],
    }


def embed(jpg: bytes):
    a = analyze(jpg)
    return a["vec"] if a else None


def save_face(uid: int, a):
    meta = json.dumps({k: a[k] for k in ("bbox", "landmarks", "det_score", "img_size")})
    with db() as c:
        c.execute("INSERT INTO faces(user_id, vec, img, meta) VALUES(?,?,?,?)",
                  (uid, a["vec"].tobytes(), a["crop"], meta))


@app.post("/api/recognize")  # ESP32-S3 CAM gọi vào đây: body = JPEG thô, header X-SHA256
async def recognize(request: Request):
    jpg = await request.body()
    want = (request.headers.get("x-sha256") or "").lower()
    if not want or hashlib.sha256(jpg).hexdigest() != want:
        return {"status": "bad_image", "reason": "sha256"}          # ảnh hỏng/cụt khi truyền
    if jpg[:2] != b"\xff\xd8" or b"\xff\xd9" not in jpg[-16:]:
        return {"status": "bad_image", "reason": "jpeg"}
    state["last_jpg"] = jpg
    a = analyze(jpg)
    if a is None:
        return {"status": "no_face"}
    v = a["vec"]
    if state["enroll_uid"] and time.time() < state["until"]:        # chế độ đăng ký bằng ESP32
        save_face(state["enroll_uid"], a)
        state["enroll_uid"] = None
        return {"status": "enrolled"}
    with db() as c:
        rows = c.execute("SELECT f.user_id, f.vec, u.name, u.locker FROM faces f JOIN users u ON u.id=f.user_id").fetchall()
    best, score = None, -1.0
    for r in rows:
        s = float(np.dot(v, np.frombuffer(r["vec"], np.float32)))
        if s > score:
            best, score = r, s
    ok = best is not None and score >= THRESH
    log(best["user_id"] if ok else None, score, int(ok), best["locker"] if ok else None, "face")
    if ok:
        if lockers.get(best["locker"], {}).get("door") != "open":   # tránh gửi lệnh mở trùng
            cmds.append({"cmd": "open", "locker": best["locker"], "by": "face"})
        return {"status": "match", "name": best["name"], "name_ascii": ascii_name(best["name"]),
                "locker": best["locker"], "score": round(score, 3)}
    return {"status": "no_match", "score": round(score, 3)}


# ---------- ESP32 DevKit ----------
@app.post("/api/device/sync")  # DevKit gửi trạng thái + sự kiện, nhận lại danh sách lệnh
def device_sync(data: dict = Body(...)):
    dev_seen["t"] = time.time()
    for l in data.get("lockers", []):
        if l.get("id") in lockers:
            lockers[l["id"]].update({k: l.get(k) for k in ("door", "item", "dist", "alarm")})
    for e in data.get("events", []):
        log(None, None, None, e.get("locker"), "device", e.get("name"))
    out = []
    while cmds:
        out.append(cmds.pop(0))
    return {"cmds": out}


@app.post("/api/finger/verify")
def finger_verify(data: dict = Body(...)):
    fid = int(data.get("finger_id") or 0)
    with db() as c:
        u = c.execute("SELECT * FROM users WHERE finger_id=?", (fid,)).fetchone() if fid else None
    if not u:
        log(None, data.get("confidence"), 0, None, "finger")
        return {"status": "no_match"}
    log(u["id"], data.get("confidence"), 1, u["locker"], "finger")
    return {"status": "match", "name": u["name"], "locker": u["locker"]}


@app.post("/api/finger/enroll_result")
def finger_enroll_result(data: dict = Body(...)):
    ok = bool(data.get("ok"))
    if ok:
        with db() as c:
            c.execute("UPDATE users SET finger_id=? WHERE id=?", (data["finger_id"], data["user_id"]))
    fp["pending"] = None
    fp["last"] = {"user_id": data.get("user_id"), "ok": ok, "reason": data.get("reason", ""), "t": time.time()}
    return {"ok": True}


@app.get("/api/state")
def get_state():
    p = fp["pending"]
    if p and time.time() - p["t"] > 120:  # quá 120s: hủy, bỏ lệnh chưa được DevKit nhận
        fp["pending"] = None
        cmds[:] = [c for c in cmds if c.get("cmd") != "enroll_finger"]
        p = None
    return {"device_online": time.time() - dev_seen["t"] < 5,
            "device_last_seen": round(time.time() - dev_seen["t"], 1) if dev_seen["t"] else None,
            "lockers": [{"id": n, **v} for n, v in lockers.items()],
            "fp_pending": p, "fp_last": fp["last"]}


# ---------- Quản trị ----------
@app.get("/api/users")
def users():
    with db() as c:
        return [dict(r) for r in c.execute(
            "SELECT u.*, (SELECT COUNT(*) FROM faces WHERE user_id=u.id) AS faces FROM users u ORDER BY u.id")]


@app.post("/api/users")
def add_user(name: str = Form(...), phone: str = Form(""), locker: int = Form(...)):
    with db() as c:
        cur = c.execute("INSERT INTO users(name,phone,locker) VALUES(?,?,?)", (name, phone, locker))
        return {"id": cur.lastrowid}


@app.delete("/api/users/{uid}")
def del_user(uid: int):
    with db() as c:
        u = c.execute("SELECT finger_id FROM users WHERE id=?", (uid,)).fetchone()
        c.execute("DELETE FROM users WHERE id=?", (uid,))
    if u and u["finger_id"]:
        cmds.append({"cmd": "delete_finger", "finger_id": u["finger_id"]})  # giải phóng ô nhớ trong cảm biến
    return {"ok": True}


@app.post("/api/users/{uid}/photo")  # đăng ký khuôn mặt bằng ảnh tải lên
async def photo(uid: int, file: UploadFile = File(...)):
    a = analyze(await file.read())
    if a is None:
        raise HTTPException(400, "Không thấy khuôn mặt trong ảnh")
    save_face(uid, a)
    return {"ok": True}


@app.post("/api/users/{uid}/enroll_esp")  # ảnh kế tiếp ESP32-S3 CAM gửi (trong 60s) được lưu cho người này
def enroll_esp(uid: int):
    state["enroll_uid"], state["until"] = uid, time.time() + 60
    return {"ok": True}


@app.post("/api/users/{uid}/enroll_finger")  # chỉ cho người đã có khuôn mặt
def enroll_finger(uid: int):
    with db() as c:
        u = c.execute("SELECT * FROM users WHERE id=?", (uid,)).fetchone()
        if not u:
            raise HTTPException(404, "Không có người dùng này")
        if c.execute("SELECT COUNT(*) FROM faces WHERE user_id=?", (uid,)).fetchone()[0] == 0:
            raise HTTPException(400, "Người dùng chưa đăng ký khuôn mặt")
        used = {r[0] for r in c.execute("SELECT finger_id FROM users WHERE finger_id IS NOT NULL")}
    fid = u["finger_id"] or next(i for i in range(1, 200) if i not in used)  # giữ ID cũ nếu đăng ký lại
    fp["pending"] = {"user_id": uid, "finger_id": fid, "t": time.time()}
    cmds.append({"cmd": "enroll_finger", "user_id": uid, "finger_id": fid})
    return {"ok": True, "finger_id": fid}


@app.get("/api/logs")
def logs():
    with db() as c:
        return [dict(r) for r in c.execute(
            "SELECT l.ts, u.name, l.score, l.matched, l.locker, l.method, l.note FROM logs l "
            "LEFT JOIN users u ON u.id=l.user_id ORDER BY l.id DESC LIMIT 20")]


@app.get("/api/snapshot")  # ảnh mới nhất ESP32-S3 CAM gửi lên
def snapshot():
    if not state["last_jpg"]:
        raise HTTPException(404)
    return Response(state["last_jpg"], media_type="image/jpeg", headers={"Cache-Control": "no-store"})


# ---------- Xuất dữ liệu khuôn mặt ----------
def _face_rows(user_id: int = 0):
    q = ("SELECT f.id, f.user_id, u.name, u.locker, f.vec, f.img, f.meta "
         "FROM faces f JOIN users u ON u.id=f.user_id")
    with db() as c:
        if user_id:
            return c.execute(q + " WHERE f.user_id=? ORDER BY f.id", (user_id,)).fetchall()
        return c.execute(q + " ORDER BY f.id").fetchall()


@app.get("/api/export/faces")  # ?fmt=json|csv|npz  &user_id=3 (tùy chọn)
def export_faces(fmt: str = "json", user_id: int = 0):
    rows = _face_rows(user_id)
    vecs = [np.frombuffer(r["vec"], np.float32) for r in rows]
    if fmt == "npz":
        buf = io.BytesIO()
        np.savez(buf,
                 face_ids=np.array([r["id"] for r in rows]),
                 user_ids=np.array([r["user_id"] for r in rows]),
                 names=np.array([r["name"] for r in rows]),
                 vecs=np.stack(vecs) if vecs else np.zeros((0, 128), np.float32))
        return Response(buf.getvalue(), media_type="application/octet-stream",
                        headers={"Content-Disposition": "attachment; filename=faces.npz"})
    if fmt == "csv":
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(["face_id", "user_id", "name", "locker"] + [f"e{i}" for i in range(128)])
        for r, v in zip(rows, vecs):
            w.writerow([r["id"], r["user_id"], r["name"], r["locker"]] + [f"{x:.6f}" for x in v])
        return Response("\ufeff" + out.getvalue(), media_type="text/csv; charset=utf-8",
                        headers={"Content-Disposition": "attachment; filename=faces.csv"})
    items = [{"face_id": r["id"], "user_id": r["user_id"], "name": r["name"], "locker": r["locker"],
              "embedding": [round(float(x), 6) for x in v],
              "meta": json.loads(r["meta"]) if r["meta"] else None}
             for r, v in zip(rows, vecs)]
    return Response(json.dumps(items, ensure_ascii=False), media_type="application/json",
                    headers={"Content-Disposition": "attachment; filename=faces.json"})


@app.get("/api/export/face_images")  # ZIP ảnh mặt đã căn chỉnh, chia thư mục theo người
def export_face_images(user_id: int = 0):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for r in _face_rows(user_id):
            if r["img"]:
                folder = f"user{r['user_id']}_{ascii_name(r['name']).replace(' ', '_')}"
                z.writestr(f"{folder}/face{r['id']}.jpg", r["img"])
    return Response(buf.getvalue(), media_type="application/zip",
                    headers={"Content-Disposition": "attachment; filename=face_images.zip"})


@app.post("/api/analyze")  # body = JPEG thô; chỉ phân tích, KHÔNG lưu. ?with_crop=1 để lấy kèm ảnh mặt base64
async def analyze_api(request: Request, with_crop: int = 0):
    a = analyze(await request.body())
    if a is None:
        return {"status": "no_face"}
    out = {"status": "ok", "bbox": a["bbox"], "landmarks": a["landmarks"], "det_score": a["det_score"],
           "embedding": [round(float(x), 6) for x in a["vec"]]}
    if with_crop:
        out["crop_b64"] = base64.b64encode(a["crop"]).decode()
    return out


app.mount("/", StaticFiles(directory="static", html=True))
