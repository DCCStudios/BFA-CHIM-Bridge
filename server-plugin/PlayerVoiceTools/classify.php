<?php
// Player Voice Tools: background worker. Asks an LLM which emotion a player line shows and
// writes "<type> <strength>\n<normalized line>" to soundcache/pvt_mood_<hash>.txt.
// Type is Skyrim's dialogue emotion: 0 neutral, 1 anger, 2 disgust, 3 fear, 4 sad, 5 happy,
// 6 surprise, 7 puzzled. Strength is 0..100.

if (PHP_SAPI !== 'cli' || !isset($argv[1])) {
    exit(1);
}

require_once(__DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'pvt_common.php');

$line = strval(base64_decode($argv[1], true));
$normalized = pvt_normalize($line);
if ($normalized === '') {
    exit(1);
}
$moodPath = pvt_mood_path($normalized);
$pendingPath = $moodPath . '.pending';

$enginePath = pvt_engine_path();
chdir($enginePath);
require_once($enginePath . 'conf' . DIRECTORY_SEPARATOR . 'conf.php');
require_once($enginePath . 'lib' . DIRECTORY_SEPARATOR . "{$GLOBALS["DBDRIVER"]}.class.php");
$db = new sql();

// Connector: conf_opts 'player_emotion_connector_id', else the first chat-completions connector.
$connectorId = 0;
$row = $db->fetchOne("SELECT value FROM conf_opts WHERE id = 'player_emotion_connector_id' LIMIT 1");
if (is_array($row)) {
    $connectorId = intval($row['value'] ?? 0);
}
$where = $connectorId > 0 ? "c.id = {$connectorId}" : "c.url LIKE '%chat/completions%'";
$connector = $db->fetchOne(
    "SELECT c.url, c.model, b.api_key FROM core_llm_connector c
     JOIN core_api_badge b ON b.id = c.api_badge_id
     WHERE {$where} ORDER BY c.id ASC LIMIT 1"
);

$result = '0 0';
if (is_array($connector) && !empty($connector['url']) && !empty($connector['model'])) {
    $types = ['neutral' => 0, 'anger' => 1, 'disgust' => 2, 'fear' => 3, 'sad' => 4, 'happy' => 5, 'surprise' => 6, 'puzzled' => 7];
    $payload = [
        'model' => $connector['model'],
        'messages' => [
            ['role' => 'system', 'content' =>
                "You label the emotion a character shows on their face while saying one line of dialogue. " .
                "Answer with one word from this list: " . implode(', ', array_keys($types)) . ". " .
                "Then a space and a strength from 40 to 100 that says how intense the emotion is. " .
                "The face is animated from this number, and low numbers barely show, so rate generously: " .
                "40-55 is a light but visible reaction (a mild question, a polite thanks), " .
                "60-75 is a clear reaction (annoyance, real curiosity, a warm greeting, suspicion), " .
                "80-90 is strong (an insult, a threat, open grief, delight), " .
                "95-100 is extreme (screaming rage, terror, sobbing). " .
                "Vary the number with the line instead of repeating one value. " .
                "Use neutral only when the face would show nothing at all. " .
                "Examples: 'What's the tax for?' -> puzzled 55. 'This is obviously a shakedown.' -> disgust 75. " .
                "'Tell me more about them.' -> puzzled 60. 'Thank you, I owe you my life!' -> happy 90. " .
                "'Get away from me or I'll kill you!' -> anger 100. 'Just passing through.' -> neutral 0. No other text."],
            ['role' => 'user', 'content' => $line],
        ],
        'max_tokens' => 12,
        'temperature' => 0,
        'stream' => false,
    ];

    $ch = curl_init($connector['url']);
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_POST, true);
    curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode($payload));
    curl_setopt($ch, CURLOPT_HTTPHEADER, [
        'Content-Type: application/json',
        'Authorization: Bearer ' . trim(strval($connector['api_key'] ?? '')),
    ]);
    curl_setopt($ch, CURLOPT_CONNECTTIMEOUT_MS, 1000);
    curl_setopt($ch, CURLOPT_TIMEOUT_MS, 4000);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);

    $answer = '';
    if ($response !== false && $httpCode === 200) {
        $decoded = json_decode($response, true);
        $answer = strtolower(trim(strval($decoded['choices'][0]['message']['content'] ?? '')));
    }
    if (preg_match('/\b(' . implode('|', array_keys($types)) . ')\b\D*(\d{1,3})?/', $answer, $matches)) {
        $type = $types[$matches[1]];
        $strength = isset($matches[2]) && $matches[2] !== '' ? max(0, min(100, intval($matches[2]))) : 50;
        $result = $type === 0 ? '0 0' : "{$type} {$strength}";
    } else {
        // No usable answer: leave nothing cached so a later line can try again.
        @unlink($pendingPath);
        exit(2);
    }
}

$staging = $moodPath . '.tmp';
file_put_contents($staging, $result . "\n" . $normalized);
rename($staging, $moodPath);
@unlink($pendingPath);
