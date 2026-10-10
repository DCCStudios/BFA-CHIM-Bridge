#include "Bridge.h"

// CHIM plays TTS through its own XAudio2 player, so the engine never speaks a voice file and
// Better Facial Animations (BFA) has nothing to animate from. For each line the CHIM server
// sends "Speaker|ScriptQueue|text/..." over a socket, then AIAgent.dll downloads
// /soundcache/<md5 of text>.wav with WinHTTP and plays it.
//
// This plugin watches both through AIAgent.dll's import table. A line whose speaker is
// "Player" marks its hash, the matching download is copied to Data\Sound\CHIMBFABridge, and
// BFA is told through Dragonborn ReVoiced's message contract (the one player-speech input BFA
// has) that the player started speaking that file.
//
// That contract carries no emotion, and BFA's player path passes a constant 0 for emotion type
// and value. The Player Voice Tools server plugin labels each player line with an LLM and
// answers ext/PlayerVoiceTools/mood.php?text=... with "<type> <strength>"; the bridge asks it
// and writes the two numbers over those constants before each line.
namespace
{
	// Mirrors DBReV_API.h (public, append-only ABI) from Dragonborn ReVoiced.
	namespace DBReV
	{
		constexpr std::uint32_t kPlayerLineStart = 1;

		struct PlayerLineStart
		{
			std::uint32_t version;
			std::uint32_t structSize;
			float         audioSeconds;
			float         totalSeconds;
			std::uint32_t topicIndex;
			std::uint32_t lipSize;
			const char*   voicePath;  // relative to Data/Sound
			const char*   voicePackId;
			const char*   topicKey;
			const void*   lipData;
		};
		static_assert(sizeof(PlayerLineStart) == 56);
	}

	using Clock = std::chrono::steady_clock;

	constexpr auto kCacheFolder = "CHIMBFABridge"sv;
	constexpr auto kPlayerMarker = "Player|ScriptQueue|"sv;
	constexpr auto kHashLifetime = 120s;

	struct Download
	{
		std::string               hash;
		std::vector<std::uint8_t> bytes;
		HINTERNET                 connect = nullptr;  // connection the audio came over
		std::wstring              moodPath;           // mood.php request for this line
	};

	std::mutex                                         g_lock;
	std::unordered_map<SOCKET, std::string>            g_streams;       // unfinished line per socket
	struct PlayerLine
	{
		Clock::time_point seen;
		std::string       text;
	};
	std::unordered_map<std::string, PlayerLine>        g_playerHashes;  // md5 hex -> spoken line
	std::unordered_map<HINTERNET, Download>            g_downloads;
	std::filesystem::path                              g_cacheDir;
	std::atomic<std::uint32_t>                         g_lineIndex{ 0 };

	// The imm32 operands of BFA's "emotion value = 0" and "emotion type = 0" for player speech.
	std::int32_t* g_bfaEmotionValue = nullptr;
	std::int32_t* g_bfaEmotionType = nullptr;

	decltype(&recv)                      g_recv = nullptr;
	decltype(&WinHttpOpenRequest)        g_openRequest = nullptr;
	decltype(&WinHttpReadData)           g_readData = nullptr;
	decltype(&WinHttpQueryDataAvailable) g_queryDataAvailable = nullptr;
	decltype(&WinHttpCloseHandle)        g_closeHandle = nullptr;

	std::string Md5Hex(std::string_view a_text)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) {
			return {};
		}
		std::uint8_t       digest[16]{};
		BCRYPT_HASH_HANDLE hash = nullptr;
		bool               ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
		ok = ok && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(a_text.data())), static_cast<ULONG>(a_text.size()), 0) >= 0;
		ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
		if (hash) {
			BCryptDestroyHash(hash);
		}
		BCryptCloseAlgorithmProvider(algorithm, 0);
		if (!ok) {
			return {};
		}
		static constexpr char kHex[] = "0123456789abcdef";
		std::string           hex;
		for (const auto byte : digest) {
			hex += kHex[byte >> 4];
			hex += kHex[byte & 0xF];
		}
		return hex;
	}

	std::string_view Trim(std::string_view a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}
		return a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
	}

	// Caller holds g_lock.
	void HandleStreamLine(std::string_view a_line)
	{
		const auto marker = a_line.find(kPlayerMarker);
		if (marker == std::string_view::npos) {
			return;
		}
		// Fields are '/' separated and the spoken text is the first one. The server names the
		// file md5(trim(text)).
		auto text = a_line.substr(marker + kPlayerMarker.size());
		text = Trim(text.substr(0, text.find('/')));
		if (text.empty()) {
			return;
		}
		const auto hash = Md5Hex(text);
		if (hash.empty()) {
			return;
		}

		const auto now = Clock::now();
		std::erase_if(g_playerHashes, [&](const auto& a_entry) { return now - a_entry.second.seen > kHashLifetime; });
		g_playerHashes[hash] = PlayerLine{ now, std::string{ text } };
		logger::info("player line '{}' -> {}", text, hash);
	}

	void ScanStream(SOCKET a_socket, const char* a_data, int a_length)
	{
		std::scoped_lock lock{ g_lock };
		if (g_streams.size() > 64 && !g_streams.contains(a_socket)) {
			g_streams.clear();
		}
		auto& pending = g_streams[a_socket];
		pending.append(a_data, static_cast<std::size_t>(a_length));

		std::size_t start = 0;
		for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n', start)) {
			HandleStreamLine(std::string_view{ pending }.substr(start, end - start));
			start = end + 1;
		}
		pending.erase(0, start);
		if (pending.size() > 16384) {
			pending.erase(0, pending.size() - 4096);
		}
	}

	// Length of the PCM data in seconds, 0 when the bytes are not a WAV file.
	float WavSeconds(const std::vector<std::uint8_t>& a_bytes)
	{
		if (a_bytes.size() < 44 || std::memcmp(a_bytes.data(), "RIFF", 4) != 0 || std::memcmp(a_bytes.data() + 8, "WAVE", 4) != 0) {
			return 0.0f;
		}
		std::uint32_t byteRate = 0;
		for (std::size_t offset = 12; offset + 8 <= a_bytes.size();) {
			std::uint32_t size = 0;
			std::memcpy(&size, a_bytes.data() + offset + 4, 4);
			const auto body = offset + 8;
			if (std::memcmp(a_bytes.data() + offset, "fmt ", 4) == 0 && body + 12 <= a_bytes.size()) {
				std::memcpy(&byteRate, a_bytes.data() + body + 8, 4);
			} else if (std::memcmp(a_bytes.data() + offset, "data", 4) == 0) {
				// A streamed encoder can leave the data size as 0 or 0xFFFFFFFF.
				const auto available = a_bytes.size() - body;
				const auto dataBytes = (size == 0 || size > available) ? available : size;
				return byteRate ? static_cast<float>(dataBytes) / static_cast<float>(byteRate) : 0.0f;
			}
			offset = body + size + (size & 1);
		}
		return 0.0f;
	}

	bool WriteCacheFile(const std::string& a_hash, const std::vector<std::uint8_t>& a_bytes)
	{
		std::error_code error;
		std::filesystem::create_directories(g_cacheDir, error);
		const auto target = g_cacheDir / (a_hash + ".wav");
		const auto staging = g_cacheDir / (a_hash + ".tmp");
		{
			std::ofstream file{ staging, std::ios::binary | std::ios::trunc };
			if (!file.write(reinterpret_cast<const char*>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()))) {
				return false;
			}
		}
		// BFA may be reading an earlier copy of the same line, so the file is swapped in whole.
		return MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
	}

	// Reads "<type> <strength>" from the server. Neutral when the file is missing or late.
	std::pair<std::int32_t, std::int32_t> FetchMood(const Download& a_download)
	{
		std::pair<std::int32_t, std::int32_t> mood{ 0, 0 };
		if (!a_download.connect || a_download.moodPath.empty()) {
			return mood;
		}
		const auto request = g_openRequest(a_download.connect, L"GET", a_download.moodPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
		if (!request) {
			return mood;
		}
		WinHttpSetTimeouts(request, 500, 500, 500, 500);
		if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
			DWORD status = 0;
			DWORD size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			char  text[32]{};
			DWORD read = 0;
			if (status == 200 && g_readData(request, text, sizeof(text) - 1, &read)) {
				int type = 0;
				int strength = 0;
				if (std::sscanf(text, "%d %d", &type, &strength) == 2 && type >= 1 && type <= 7) {
					mood = { type, std::clamp(strength, 0, 100) };
				}
			}
		}
		g_closeHandle(request);
		return mood;
	}

	// BFA reads these when it starts the line on the game thread, after the dispatch below.
	bool SetBfaEmotion(std::pair<std::int32_t, std::int32_t> a_mood)
	{
		if (!g_bfaEmotionType || !g_bfaEmotionValue) {
			return false;
		}
		*g_bfaEmotionValue = a_mood.second;
		*g_bfaEmotionType = a_mood.first;
		return true;
	}

	void FinishDownload(HINTERNET a_request)
	{
		Download download;
		{
			std::scoped_lock lock{ g_lock };
			const auto       it = g_downloads.find(a_request);
			if (it == g_downloads.end()) {
				return;
			}
			download = std::move(it->second);
			g_downloads.erase(it);
		}

		const auto seconds = WavSeconds(download.bytes);
		if (seconds <= 0.0f) {
			logger::warn("download {} is not a WAV file ({} bytes), nothing sent", download.hash, download.bytes.size());
			return;
		}
		if (!WriteCacheFile(download.hash, download.bytes)) {
			logger::error("could not write {}.wav to {}", download.hash, g_cacheDir.string());
			return;
		}

		const auto mood = FetchMood(download);
		const bool moodSet = SetBfaEmotion(mood);

		const auto             voicePath = std::string{ kCacheFolder } + "\\" + download.hash + ".wav";
		DBReV::PlayerLineStart line{};
		line.version = 1;
		line.structSize = sizeof(line);
		line.audioSeconds = seconds;
		line.totalSeconds = seconds;
		line.topicIndex = ++g_lineIndex;
		line.voicePath = voicePath.c_str();
		line.voicePackId = "CHIM";
		line.topicKey = "";
		const auto sent = SKSE::GetMessagingInterface()->Dispatch(DBReV::kPlayerLineStart, &line, sizeof(line), nullptr);
		logger::info("player speech {} ({:.2f} s, emotion {}/{}{}) {}", voicePath, seconds, mood.first, mood.second,
			moodSet ? "" : " not applied", sent ? "sent to listeners" : "had no listener");
	}

	int WSAAPI RecvHook(SOCKET a_socket, char* a_buffer, int a_length, int a_flags)
	{
		const auto received = g_recv(a_socket, a_buffer, a_length, a_flags);
		if (received > 0 && !(a_flags & MSG_PEEK)) {
			ScanStream(a_socket, a_buffer, received);
		}
		return received;
	}

	HINTERNET WINAPI OpenRequestHook(HINTERNET a_connect, LPCWSTR a_verb, LPCWSTR a_object, LPCWSTR a_version, LPCWSTR a_referrer, LPCWSTR* a_acceptTypes, DWORD a_flags)
	{
		const auto request = g_openRequest(a_connect, a_verb, a_object, a_version, a_referrer, a_acceptTypes, a_flags);
		if (!request || !a_object) {
			return request;
		}

		// /HerikaServer/soundcache/<32 hex>.wav
		const std::wstring_view object{ a_object };
		const auto              folder = object.find(L"/soundcache/");
		const auto              extension = object.rfind(L".wav");
		if (folder == std::wstring_view::npos || extension == std::wstring_view::npos || extension < folder + 12) {
			return request;
		}
		std::string hash;
		for (const auto c : object.substr(folder + 12, extension - folder - 12)) {
			hash += static_cast<char>(std::tolower(static_cast<int>(c & 0x7F)));
		}

		std::scoped_lock lock{ g_lock };
		const auto       it = g_playerHashes.find(hash);
		if (it != g_playerHashes.end() && Clock::now() - it->second.seen <= kHashLifetime) {
			// <server root>/ext/PlayerVoiceTools/mood.php?text=<percent-encoded UTF-8 line>
			auto moodPath = std::wstring{ object.substr(0, folder) } + L"/ext/PlayerVoiceTools/mood.php?text=";
			static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
			for (const unsigned char c : it->second.text) {
				if (std::isalnum(c)) {
					moodPath += static_cast<wchar_t>(c);
				} else {
					moodPath += L'%';
					moodPath += kHex[c >> 4];
					moodPath += kHex[c & 0xF];
				}
			}
			g_downloads[request] = Download{ hash, {}, a_connect, std::move(moodPath) };
		}
		return request;
	}

	BOOL WINAPI ReadDataHook(HINTERNET a_request, LPVOID a_buffer, DWORD a_toRead, LPDWORD a_read)
	{
		const auto ok = g_readData(a_request, a_buffer, a_toRead, a_read);
		if (!ok || !a_read) {
			return ok;
		}
		if (*a_read == 0) {
			FinishDownload(a_request);
			return ok;
		}
		std::scoped_lock lock{ g_lock };
		if (const auto it = g_downloads.find(a_request); it != g_downloads.end()) {
			const auto bytes = static_cast<const std::uint8_t*>(a_buffer);
			it->second.bytes.insert(it->second.bytes.end(), bytes, bytes + *a_read);
		}
		return ok;
	}

	BOOL WINAPI QueryDataAvailableHook(HINTERNET a_request, LPDWORD a_available)
	{
		const auto ok = g_queryDataAvailable(a_request, a_available);
		if (ok && a_available && *a_available == 0) {
			FinishDownload(a_request);
		}
		return ok;
	}

	BOOL WINAPI CloseHandleHook(HINTERNET a_handle)
	{
		FinishDownload(a_handle);
		return g_closeHandle(a_handle);
	}

	// Replaces every import-table slot of a_module that points at a_original.
	int PatchImports(HMODULE a_module, void* a_original, void* a_replacement)
	{
		const auto base = reinterpret_cast<std::uint8_t*>(a_module);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!directory.VirtualAddress) {
			return 0;
		}

		int patched = 0;
		for (auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); descriptor->Name; ++descriptor) {
			for (auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk); thunk->u1.Function; ++thunk) {
				if (reinterpret_cast<void*>(thunk->u1.Function) != a_original) {
					continue;
				}
				DWORD protection = 0;
				if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &protection)) {
					thunk->u1.Function = reinterpret_cast<ULONGLONG>(a_replacement);
					VirtualProtect(&thunk->u1.Function, sizeof(void*), protection, &protection);
					++patched;
				}
			}
		}
		return patched;
	}

	template <class T>
	bool Hook(HMODULE a_target, const wchar_t* a_library, const char* a_name, T& a_original, T a_replacement)
	{
		const auto library = GetModuleHandleW(a_library);
		a_original = library ? reinterpret_cast<T>(GetProcAddress(library, a_name)) : nullptr;
		const auto patched = a_original ? PatchImports(a_target, reinterpret_cast<void*>(a_original), reinterpret_cast<void*>(a_replacement)) : 0;
		if (patched == 0) {
			logger::error("AIAgent.dll import {} not found", a_name);
		}
		return patched > 0;
	}

	// Finds the player-speech call in BetterFacialAnimations.dll:
	//   mov dword [rsp+28h], 0   ; emotion value
	//   mov dword [rsp+20h], 0   ; emotion type
	//   movups [rsp+70h], xmm4
	//   call BeginSpeech
	// with "mov r8d, 0FFFFFFFFh" (no sound id) shortly before. Anything else is left alone.
	bool LocateBfaEmotion()
	{
		const auto module = GetModuleHandleW(L"BetterFacialAnimations.dll");
		if (!module) {
			logger::warn("BetterFacialAnimations.dll is not loaded, emotions are off");
			return false;
		}
		static constexpr std::uint8_t kCall[] = { 0x0F, 0x11, 0x64, 0x24, 0x70, 0xE8 };
		static constexpr std::uint8_t kNoSound[] = { 0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF };

		const auto base = reinterpret_cast<std::uint8_t*>(module);
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
		const auto sections = IMAGE_FIRST_SECTION(nt);
		std::uint8_t* found = nullptr;
		int           matches = 0;
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || sections[i].Misc.VirtualSize < 0x80) {
				continue;
			}
			const auto begin = base + sections[i].VirtualAddress;
			const auto end = begin + sections[i].Misc.VirtualSize - 0x20;
			for (auto p = begin + 0x40; p < end; ++p) {
				if (p[0] != 0xC7 || p[1] != 0x44 || p[2] != 0x24 || p[3] != 0x28 ||
					p[8] != 0xC7 || p[9] != 0x44 || p[10] != 0x24 || p[11] != 0x20 ||
					std::memcmp(p + 16, kCall, sizeof(kCall)) != 0) {
					continue;
				}
				if (std::search(p - 0x40, p, std::begin(kNoSound), std::end(kNoSound)) == p) {
					continue;
				}
				found = p;
				++matches;
			}
		}
		std::int32_t value = -1;
		std::int32_t type = -1;
		if (found) {
			std::memcpy(&value, found + 4, 4);
			std::memcpy(&type, found + 12, 4);
		}
		if (matches != 1 || value != 0 || type != 0) {
			logger::warn("BFA's player emotion constants were not found ({} candidates), emotions are off", matches);
			return false;
		}
		DWORD protection = 0;
		if (!VirtualProtect(found, 16, PAGE_EXECUTE_READWRITE, &protection)) {
			return false;
		}
		g_bfaEmotionValue = reinterpret_cast<std::int32_t*>(found + 4);
		g_bfaEmotionType = reinterpret_cast<std::int32_t*>(found + 12);
		logger::info("BFA player emotion constants found at +{:X}", static_cast<std::size_t>(found - base));
		return true;
	}

	// Removes this plugin's own files from earlier sessions: <32 hex>.wav and .tmp in its folder.
	void ClearCache()
	{
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator{ g_cacheDir, error }) {
			const auto stem = entry.path().stem().string();
			const auto extension = entry.path().extension().string();
			const auto isHash = stem.size() == 32 && std::ranges::all_of(stem, [](unsigned char c) { return std::isxdigit(c) != 0; });
			if (entry.is_regular_file(error) && isHash && (extension == ".wav" || extension == ".tmp")) {
				std::filesystem::remove(entry.path(), error);
			}
		}
	}
}

bool Bridge::Install()
{
	const auto agent = GetModuleHandleW(L"AIAgent.dll");
	if (!agent) {
		logger::warn("AIAgent.dll is not loaded, nothing to bridge");
		return false;
	}

	wchar_t executable[MAX_PATH]{};
	GetModuleFileNameW(nullptr, executable, MAX_PATH);
	g_cacheDir = std::filesystem::path{ executable }.parent_path() / "Data" / "Sound" / kCacheFolder;
	ClearCache();

	const bool stream = Hook(agent, L"ws2_32.dll", "recv", g_recv, &RecvHook);
	bool       http = Hook(agent, L"winhttp.dll", "WinHttpOpenRequest", g_openRequest, &OpenRequestHook);
	http = Hook(agent, L"winhttp.dll", "WinHttpReadData", g_readData, &ReadDataHook) && http;
	http = Hook(agent, L"winhttp.dll", "WinHttpCloseHandle", g_closeHandle, &CloseHandleHook) && http;
	Hook(agent, L"winhttp.dll", "WinHttpQueryDataAvailable", g_queryDataAvailable, &QueryDataAvailableHook);

	LocateBfaEmotion();
	logger::info("cache folder {}", g_cacheDir.string());
	return stream && http;
}
