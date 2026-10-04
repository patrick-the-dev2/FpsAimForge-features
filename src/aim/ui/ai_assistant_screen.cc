
  void LoadMemory() {
    const auto path = MemoryPath();
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    std::string magic;
    std::getline(input, magic);
    if (magic != "FPSAIMFORGE_AI_CHAT_V1") return;
    while (input.good()) {
      char role = 0;
      input.get(role);
      if (!input || (role != 'U' && role != 'A')) break;
      if (input.peek() != ' ') break;
      input.get();
      std::string size_text;
      std::getline(input, size_text);
      if (size_text.empty()) break;
      size_t size = 0;
      try {
        size = std::stoull(size_text);
      } catch (...) {
        break;
      }
      std::string text(size, '\0');
      input.read(text.data(), static_cast<std::streamsize>(size));
      if (input.gcount() != static_cast<std::streamsize>(size)) break;