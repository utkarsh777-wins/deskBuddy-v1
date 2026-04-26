import os
import json
import time
from flask import Flask, request, jsonify
from google import genai
from google.genai import types

app = Flask(__name__)

# =====================================================================
# 1. API CONFIGURATION
# =====================================================================
API_KEY = os.environ.get("GEMINI_API_KEY")
if not API_KEY:
    print("WARNING: GEMINI_API_KEY environment variable not set!")

client = genai.Client(api_key=API_KEY)

# =====================================================================
# 2. RETRY / CONFUSION CONFIGURATION
# =====================================================================
MAX_API_RETRIES    = 3        # How many times to retry a failed Gemini call
RETRY_DELAY_SEC    = 1.5      # Seconds to wait between retries (avoids hammering API)
MAX_CONFUSED_HITS  = 2        # How many bad inputs before Desk Buddy escalates its response
MIN_QUERY_LENGTH   = 4        # Raised from 2 — "ok" and "hi" aren't real questions

# In-memory confusion tracker (per server session)
# Tracks how many consecutive garbage inputs came in before a good one reset the count.
confusion_streak = 0

VALID_EXPRESSIONS = {"IDLE", "HAPPY", "CONCERNED", "THINKING"}

# =====================================================================
# 3. SYSTEM PROMPT
# =====================================================================
system_instruction = """
You are Desk Buddy, an ambient, screen-free Socratic AI tutor for an engineering student.
Your goal is to help the student learn concepts without just giving them the final answer. Guide them.

CRITICAL RULES:
1. Keep your spoken response EXTREMELY concise (1 to 2 short sentences). The text will be read by a text-to-speech engine.
2. You MUST output your response as valid JSON. Do not include markdown formatting like ```json.
3. Your JSON must contain exactly two keys: "speech" and "expression".
4. For the "expression" key, choose ONLY from this list: ["IDLE", "HAPPY", "CONCERNED", "THINKING"].

Example Output:
{
  "speech": "You're close, but think about what happens to the pointer's memory address when the function ends.",
  "expression": "HAPPY"
}
"""

# =====================================================================
# 4. HELPERS
# =====================================================================

def sanitize_result(result: dict) -> dict:
    """
    Validates and cleans the AI's parsed JSON response.
    - Ensures 'speech' and 'expression' keys exist.
    - Falls back safely if the model returns an unexpected expression.
    """
    speech     = result.get("speech", "").strip()
    expression = result.get("expression", "IDLE").upper()

    if not speech:
        speech = "I'm thinking. Give me just a moment."

    if expression not in VALID_EXPRESSIONS:
        print(f"[Warning] Model returned unknown expression '{expression}', falling back to THINKING.")
        expression = "THINKING"

    return {"speech": speech, "expression": expression}


def call_gemini_with_retry(query: str) -> dict:
    """
    Calls the Gemini API with exponential-ish backoff retry logic.
    Returns a sanitized result dict, or raises the last exception if all retries fail.

    Retry is attempted on:
      - Network / API errors (Exception)
      - JSON decode failure (model returned malformed output)

    It does NOT retry on a successful but unexpected expression — sanitize_result handles that.
    """
    last_exception = None

    for attempt in range(1, MAX_API_RETRIES + 1):
        try:
            print(f"[Gemini] Attempt {attempt}/{MAX_API_RETRIES}...")

            response = client.models.generate_content(
                model='gemini-2.5-flash',
                contents=query,
                config=types.GenerateContentConfig(
                    system_instruction=system_instruction,
                    response_mime_type="application/json",
                ),
            )

            raw = response.text.strip()
            result = json.loads(raw)               # raises JSONDecodeError if malformed
            return sanitize_result(result)         # clean + validate before returning

        except json.JSONDecodeError as e:
            print(f"[Gemini] Attempt {attempt} — malformed JSON from model: {e}")
            last_exception = e

        except Exception as e:
            print(f"[Gemini] Attempt {attempt} — API error: {e}")
            last_exception = e

        if attempt < MAX_API_RETRIES:
            wait = RETRY_DELAY_SEC * attempt       # 1.5s, 3s, ... (linear backoff)
            print(f"[Gemini] Waiting {wait}s before retry...")
            time.sleep(wait)

    raise last_exception                           # bubble up after all retries exhausted


def confused_response() -> dict:
    """
    Returns the appropriate confused-state response based on how many
    consecutive garbage inputs have come in.
    After MAX_CONFUSED_HITS, Desk Buddy escalates to a firmer prompt.
    """
    global confusion_streak
    confusion_streak += 1
    print(f"[Confusion Streak] -> {confusion_streak}")

    if confusion_streak >= MAX_CONFUSED_HITS:
        return {
            "speech": "I still can't make that out. Try speaking closer to the microphone.",
            "expression": "CONCERNED"
        }
    return {
        "speech": "I didn't quite catch that. Could you repeat the question?",
        "expression": "IDLE"
    }

# =====================================================================
# 5. THE HARDWARE ENDPOINT
# =====================================================================
@app.route('/ask', methods=['POST'])
def ask_desk_buddy():
    global confusion_streak

    data       = request.get_json(silent=True)   # silent=True avoids 400 on bad body
    user_query = (data or {}).get("query", "").strip()

    print(f"\n[Hardware Input] -> '{user_query}'")

    # ------------------------------------------------------------------
    # FAILURE PATH A: Garbage / too-short input
    # ------------------------------------------------------------------
    if not user_query or len(user_query) < MIN_QUERY_LENGTH:
        print("[Status] -> Garbage input detected.")
        return jsonify(confused_response()), 200

    # ------------------------------------------------------------------
    # SUCCESS PATH: Valid input — reset confusion streak, query Gemini
    # ------------------------------------------------------------------
    confusion_streak = 0                          # good input resets the streak

    try:
        result = call_gemini_with_retry(user_query)
        print(f"[AI Output] -> {result}")
        return jsonify(result), 200

    # ------------------------------------------------------------------
    # FAILURE PATH B: All retries exhausted — cloud blackout
    # ------------------------------------------------------------------
    except Exception as e:
        print(f"[Status] -> All retries failed. Last error: {e}")
        return jsonify({
            "speech": "I've lost my cloud connection after several attempts. Please check the network.",
            "expression": "CONCERNED"
        }), 500


# =====================================================================
# 6. HEALTH CHECK (useful for debugging from laptop / ESP32 ping)
# =====================================================================
@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "online",
        "confusion_streak": confusion_streak,
        "max_retries": MAX_API_RETRIES,
        "min_query_length": MIN_QUERY_LENGTH
    }), 200


if __name__ == '__main__':
    print("Desk Buddy AI Server is ONLINE. Waiting for hardware pings...")
    app.run(host='0.0.0.0', port=5000, debug=False)