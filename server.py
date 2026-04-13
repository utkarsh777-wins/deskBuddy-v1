import os
import json
from flask import Flask, request, jsonify
from google import genai
from google.genai import types

app = Flask(__name__)

# =====================================================================
# 1. API CONFIGURATION
# Pulls the key securely from the terminal environment
# =====================================================================
API_KEY = os.environ.get("GEMINI_API_KEY")
if not API_KEY:
    print("WARNING: GEMINI_API_KEY environment variable not set!")

# Initialize the modern genai client
client = genai.Client(api_key=API_KEY)

# =====================================================================
# 2. THE SOCRATIC SYSTEM PROMPT
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
# 3. THE HARDWARE ENDPOINT
# =====================================================================
@app.route('/ask', methods=['POST'])
def ask_desk_buddy():
    data = request.get_json()
    user_query = data.get("query", "").strip()
    
    print(f"\n[Hardware Input] -> {user_query}")

    # FAILURE PATH A: The "Garbage Input" Bailout
    if not user_query or len(user_query) < 2:
        print("[Status] -> Triggering CONFUSED bailout.")
        return jsonify({
            "speech": "I didn't quite catch that. Could you repeat the question?",
            "expression": "CONFUSED"
        }), 200

    # SUCCESS PATH: Query the modern Gemini API
    try:
        print("[Status] -> Pinging Gemini API...")
        response = client.models.generate_content(
            model='gemini-2.5-flash',
            contents=user_query,
            config=types.GenerateContentConfig(
                system_instruction=system_instruction,
                response_mime_type="application/json",
            ),
        )
        
        # Parse the AI's response to ensure it's valid JSON
        result = json.loads(response.text)
        print(f"[AI Output] -> {result}")
        
        return jsonify(result), 200

    except Exception as e:
        # FAILURE PATH B: The "Cloud Blackout" Bailout
        print(f"[Status] -> API Error: {e}")
        return jsonify({
            "speech": "I seem to have lost my connection to the cloud. Give me a second to reconnect.",
            "expression": "ERROR"
        }), 500


if __name__ == '__main__':
    print("Desk Buddy AI Server is ONLINE. Waiting for hardware pings...")
    app.run(host='0.0.0.0', port=5000, debug=True)