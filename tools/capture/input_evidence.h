#ifndef ENGINE_SIM_CAPTURE_INPUT_EVIDENCE_H
#define ENGINE_SIM_CAPTURE_INPUT_EVIDENCE_H

#include "compiler.h"
#include <fstream>

namespace engine_sim_capture {
// Retain the bytes owned by completed preparation, rather than reopening a
// pathname after rendering. All serialization belongs to the tool, not render.
class InputEvidence {
public:
    explicit InputEvidence(const std::filesystem::path &out)
        : m_out(out), m_paths(out / "inputs.txt", std::ios::binary),
          m_map(out / "prepared-inputs-v2.tsv", std::ios::binary) {
        m_paths.exceptions(std::ios::badbit | std::ios::failbit);
        m_map.exceptions(std::ios::badbit | std::ios::failbit);
    }
    void append(const std::vector<es_script::ScriptInput> &inputs) {
        for (const auto &input : inputs) {
            const auto prefix = "prepared-input-" + std::to_string(m_index);
            write(m_out / (prefix + ".bin"), input.bytes);
            write(m_out / (prefix + ".path"), input.resolvedPath.u8string());
            m_paths << input.resolvedPath.u8string() << '\n';
            m_map << m_index++ << '\t' << input.bytes.size() << '\n';
        }
        m_paths.flush(); m_map.flush();
    }
private:
    static void write(const std::filesystem::path &path, const std::string &bytes) {
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
    }
    std::filesystem::path m_out;
    std::ofstream m_paths, m_map;
    std::size_t m_index = 0;
};
}
#endif
