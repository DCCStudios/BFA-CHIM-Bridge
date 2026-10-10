<?php
// Player Voice Tools: lets Cartesia and Inworld use voices made on the provider's site.
//
// CHIM's Cartesia and Inworld connectors treat a voice setting as the name of a local sample
// to clone, and look it up in a cache (conf_opts) before anything else. This hook fills that
// cache for configured voices that have no local sample:
//   - a Cartesia voice ID (UUID) maps to itself
//   - an Inworld voice ID (workspace__voice) maps to itself
//   - any other name is looked up once on the provider (Cartesia library, Inworld voice list)
// so the stock connector code finds a ready voice ID and never tries to clone.

if (!function_exists('pvt_seed_voice_ids')) {
    function pvt_voice_opt_exists($db, string $id): bool
    {
        $row = $db->fetchOne("SELECT value FROM conf_opts WHERE id = '" . $db->escape($id) . "' LIMIT 1");
        return is_array($row) && trim(strval($row['value'] ?? '')) !== '';
    }

    function pvt_voice_opt_set($db, string $id, string $value): void
    {
        $idEscaped = $db->escape($id);
        $valueEscaped = $db->escape($value);
        $db->execQuery("INSERT INTO conf_opts (id, value) VALUES ('{$idEscaped}', '{$valueEscaped}') ON CONFLICT(id) DO UPDATE SET value = '{$valueEscaped}'");
    }

    function pvt_http_json(string $url, array $headers): ?array
    {
        $ch = curl_init($url);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_HTTPHEADER, $headers);
        curl_setopt($ch, CURLOPT_CONNECTTIMEOUT_MS, 1500);
        curl_setopt($ch, CURLOPT_TIMEOUT_MS, 5000);
        $response = curl_exec($ch);
        $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);
        $decoded = ($response !== false && $httpCode === 200) ? json_decode($response, true) : null;
        return is_array($decoded) ? $decoded : null;
    }

    // Same scoped cache key the Inworld connector builds from its connector id and workspace.
    function pvt_inworld_scoped_key(array $connector, string $voice): string
    {
        $hash = substr(md5(json_encode([
            'connector_id' => intval($connector['id']),
            'workspace' => strtolower($connector['workspace']),
        ])), 0, 12);
        return "inworld_voice_scope_{$hash}__{$voice}";
    }

    function pvt_find_cartesia_library_voice(string $name, string $apiKey): string
    {
        $needle = preg_replace('/[^a-z0-9]+/', '', strtolower($name));
        $result = pvt_http_json(
            'https://api.cartesia.ai/voices?' . http_build_query(['limit' => 100, 'q' => $name], '', '&', PHP_QUERY_RFC3986),
            ["Authorization: Bearer {$apiKey}", "X-API-Key: {$apiKey}", 'Cartesia-Version: 2026-03-01', 'Accept: application/json']
        );
        foreach (($result['voices'] ?? $result['data'] ?? []) as $voice) {
            if (is_array($voice) && preg_replace('/[^a-z0-9]+/', '', strtolower(strval($voice['name'] ?? ''))) === $needle) {
                return trim(strval($voice['id'] ?? ''));
            }
        }
        return '';
    }

    function pvt_find_inworld_voice(string $name, string $apiKey): string
    {
        $needle = preg_replace('/[^a-z0-9]+/', '', strtolower($name));
        $result = pvt_http_json(
            'https://api.inworld.ai/voices/v1/voices?' . http_build_query(['pageSize' => 2000], '', '&', PHP_QUERY_RFC3986),
            ["Authorization: Basic {$apiKey}"]
        );
        foreach (($result['voices'] ?? []) as $voice) {
            if (!is_array($voice)) {
                continue;
            }
            $parts = explode('/', strval($voice['name'] ?? ''));
            foreach ([$voice['displayName'] ?? '', $voice['voiceId'] ?? '', end($parts)] as $candidate) {
                if (preg_replace('/[^a-z0-9]+/', '', strtolower(strval($candidate))) === $needle) {
                    return trim(strval($voice['voiceId'] ?? ''));
                }
            }
        }
        return '';
    }

    function pvt_seed_voice_ids(): void
    {
        $db = $GLOBALS["db"] ?? null;
        if (!$db) {
            return;
        }
        $enginePath = dirname(__DIR__, 2) . DIRECTORY_SEPARATOR;

        // A voice typed into a request (the TTS Connector test) is always handled. Voices
        // saved in settings are rescanned at most once a minute.
        $candidates = [];
        foreach (['voiceid', 'voice_override'] as $field) {
            foreach ([$_POST[$field] ?? '', $_GET[$field] ?? ''] as $value) {
                $candidates[] = strval($value);
            }
        }
        $stamp = $enginePath . 'soundcache' . DIRECTORY_SEPARATOR . 'pvt_voice_scan.stamp';
        if (!is_file($stamp) || time() - filemtime($stamp) >= 60) {
            @touch($stamp);
            foreach ($db->fetchAll("SELECT value FROM core_player WHERE id IN ('tts_voice_override', 'tts_voice_id_override')") ?: [] as $row) {
                $candidates[] = strval($row['value'] ?? '');
            }
            foreach ($db->fetchAll("SELECT DISTINCT voiceid FROM core_npc_master WHERE COALESCE(voiceid, '') <> ''") ?: [] as $row) {
                $candidates[] = strval($row['voiceid'] ?? '');
            }
        }
        $candidates = array_unique(array_filter(array_map('trim', $candidates), function ($voice) use ($enginePath) {
            return $voice !== '' && strlen($voice) <= 120 && !preg_match('/[\/\\\\\'"]/', $voice)
                && !is_file($enginePath . 'data' . DIRECTORY_SEPARATOR . 'voices' . DIRECTORY_SEPARATOR . $voice . '.wav');
        }));
        if (empty($candidates)) {
            return;
        }

        $cartesiaKey = '';
        $inworldConnectors = [];
        foreach ($db->fetchAll(
            "SELECT c.id, c.driver, c.metadata, b.api_key FROM core_tts_connector c
             LEFT JOIN core_api_badge b ON b.id = c.api_badge_id
             WHERE c.driver IN ('cartesia', 'inworld')"
        ) ?: [] as $row) {
            $apiKey = trim(strval($row['api_key'] ?? ''));
            if ($row['driver'] === 'cartesia' && $cartesiaKey === '') {
                $cartesiaKey = $apiKey;
            } elseif ($row['driver'] === 'inworld') {
                $metadata = json_decode(strval($row['metadata'] ?? '{}'), true);
                $workspace = trim(strval($metadata['workspace'] ?? ''));
                if (strpos($workspace, 'workspaces/') === 0) {
                    $workspace = substr($workspace, strlen('workspaces/'));
                }
                $inworldConnectors[] = ['id' => intval($row['id']), 'workspace' => trim($workspace, "/ \t\n\r\0\x0B"), 'api_key' => $apiKey];
            }
        }

        $lookups = 0;
        foreach ($candidates as $voice) {
            // Cartesia voice ID.
            if (preg_match('/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i', $voice)) {
                if (!pvt_voice_opt_exists($db, "cartesia_voice_id_{$voice}")) {
                    pvt_voice_opt_set($db, "cartesia_voice_id_{$voice}", strtolower($voice));
                }
                continue;
            }
            // Inworld voice ID.
            if (strpos($voice, '__') !== false) {
                foreach ($inworldConnectors as $connector) {
                    $key = pvt_inworld_scoped_key($connector, $voice);
                    if (!pvt_voice_opt_exists($db, $key)) {
                        pvt_voice_opt_set($db, $key, $voice);
                    }
                }
                continue;
            }

            // A name: one provider lookup per name, remembered for a day when nothing is found.
            if ($cartesiaKey !== '' && !pvt_voice_opt_exists($db, "cartesia_voice_id_{$voice}")) {
                $missKey = "pvt_voice_miss_cartesia_{$voice}";
                $miss = $db->fetchOne("SELECT value FROM conf_opts WHERE id = '" . $db->escape($missKey) . "' LIMIT 1");
                if ((!is_array($miss) || time() - intval($miss['value'] ?? 0) > 86400) && $lookups < 3) {
                    $lookups++;
                    $found = pvt_find_cartesia_library_voice($voice, $cartesiaKey);
                    pvt_voice_opt_set($db, $found !== '' ? "cartesia_voice_id_{$voice}" : $missKey, $found !== '' ? $found : strval(time()));
                }
            }
            foreach ($inworldConnectors as $connector) {
                $key = pvt_inworld_scoped_key($connector, $voice);
                if ($connector['api_key'] === '' || pvt_voice_opt_exists($db, $key)) {
                    continue;
                }
                $missKey = "pvt_voice_miss_inworld_{$connector['id']}_{$voice}";
                $miss = $db->fetchOne("SELECT value FROM conf_opts WHERE id = '" . $db->escape($missKey) . "' LIMIT 1");
                if ((!is_array($miss) || time() - intval($miss['value'] ?? 0) > 86400) && $lookups < 3) {
                    $lookups++;
                    $found = pvt_find_inworld_voice($voice, $connector['api_key']);
                    pvt_voice_opt_set($db, $found !== '' ? $key : $missKey, $found !== '' ? $found : strval(time()));
                }
            }
        }
    }
}

try {
    pvt_seed_voice_ids();
} catch (Throwable $pvtError) {
    error_log('[PlayerVoiceTools] voice ID seeding failed: ' . $pvtError->getMessage());
}
