#include "cli.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace engine_sim::legacy::cli {
namespace {
constexpr std::size_t journal_limit = 128 * 1024 * 1024;

[[noreturn]] void io(std::string code, std::string reason, const fs::path &path) {
    throw Failure(6,"output",std::move(code),std::move(reason),path);
}

fs::file_status destination_status(const fs::path &path) {
    std::error_code error;
    const auto found=fs::symlink_status(path,error);
    if (error && found.type()!=fs::file_type::not_found)
        io("CONVERT_OUTPUT",error.message(),path);
    return found;
}

bool occupied(const fs::path &path) {
    return fs::exists(destination_status(path));
}

class DirectoryLock {
public:
    explicit DirectoryLock(const fs::path &root) {
#ifdef _WIN32
        const auto path=root/L".engine-sim-converter.lock";
        handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (handle==INVALID_HANDLE_VALUE)
            io("CONVERT_BUSY","Cannot lock the destination directory",root);
#else
        descriptor=open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if (descriptor<0) io("CONVERT_OUTPUT","Cannot open the destination directory",root);
        if (flock(descriptor,LOCK_EX|LOCK_NB)<0) {
            close(descriptor);
            io("CONVERT_BUSY","Cannot lock the destination directory",root);
        }
#endif
    }
    DirectoryLock(const DirectoryLock &)=delete;
    DirectoryLock &operator=(const DirectoryLock &)=delete;
    ~DirectoryLock() {
#ifdef _WIN32
        CloseHandle(handle);
#else
        close(descriptor);
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
#else
    int descriptor=-1;
#endif
};

class Temporary {
public:
    explicit Temporary(const fs::path &destination) {
        std::random_device random;
        for (int attempt=0;attempt<16;++attempt) {
            path=destination.parent_path()/(".engine-sim-"+std::to_string(random())+"-"+std::to_string(random())+".tmp");
#ifdef _WIN32
            stream=_wfopen(path.c_str(),L"wbx");
#else
            const auto fd=open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
            if (fd>=0) {
                stream=fdopen(fd,"wb");
                if (!stream) {
                    close(fd);
                    std::error_code ignored;
                    fs::remove(path,ignored);
                    io("CONVERT_OUTPUT","Cannot open a temporary output stream",destination);
                }
            }
#endif
            if (stream) return;
            if (errno!=EEXIST) io("CONVERT_OUTPUT","Cannot create a temporary sibling",destination);
        }
        io("CONVERT_OUTPUT","Cannot find an unused temporary sibling",destination);
    }
    Temporary(const Temporary &)=delete;
    Temporary &operator=(const Temporary &)=delete;
    ~Temporary() {
        if (stream) std::fclose(stream);
        std::error_code ignored;
        if (!path.empty()) fs::remove(path,ignored);
    }
    void finish(std::string_view bytes) {
        if (std::fwrite(bytes.data(),1,bytes.size(),stream)!=bytes.size() || std::fflush(stream)!=0)
            io("CONVERT_SHORT_WRITE","Cannot write the complete temporary payload",path);
#ifdef _WIN32
        if (_commit(_fileno(stream))!=0)
#else
        if (fsync(fileno(stream))!=0)
#endif
            io("CONVERT_SHORT_WRITE","Cannot flush the complete temporary payload",path);
        if (std::fclose(std::exchange(stream,nullptr))!=0)
            io("CONVERT_SHORT_WRITE","Cannot close the complete temporary payload",path);
        if (sha256_file(path)!=sha256(bytes))
            io("CONVERT_SHORT_WRITE","Temporary payload differs after reading it back",path);
    }

    fs::path path;
private:
    std::FILE *stream=nullptr;
};

void store(const fs::path &path, const Json &value, bool replace) {
    publish(path,value.dump(2)+'\n',replace);
}

class Journal {
public:
    Journal(const Plan &plan, const std::string &executable_hash)
        : path(plan.manifest), identity(sha256(plan_identity(plan)+executable_hash)) {
        path += ".journal";
        std::set<fs::path> roots{plan.manifest.parent_path()};
        for (const auto &item : plan.items) {
            std::error_code error;
            if (item.output && fs::is_directory(item.output->parent_path(),error))
                roots.insert(item.output->parent_path());
        }
        for (const auto &root : roots) locks.push_back(std::make_unique<DirectoryLock>(root));
        if (occupied(path)) {
            if (!fs::is_regular_file(destination_status(path))) io("CONVERT_COLLISION","Journal path is not a regular file",path);
            data=Json::parse(read_file(path,journal_limit,6),nullptr,false);
            if (!data.is_object() || data.value("version",0)!=1 || data.value("identity","")!=identity ||
                !data.contains("entries") || !data["entries"].is_array() || data["entries"].size()!=plan.items.size())
                io("CONVERT_COLLISION","Existing journal belongs to a different or malformed invocation",path);
            resumed=true;
        }
        else {
            if (occupied(plan.manifest) && !plan.force)
                io("CONVERT_COLLISION","Manifest already exists",plan.manifest);
            data={{"version",1},{"identity",identity},{"entries",Json::array()}};
            for (std::size_t i=0;i<plan.items.size();++i) data["entries"].push_back(nullptr);
            store(path,data,false);
        }
    }

    Json &at(std::size_t i) { return data.at("entries").at(i); }
    void save() { store(path,data,true); }
    bool was_interrupted() const noexcept { return resumed; }
    const fs::path &filename() const noexcept { return path; }
    void close() {
        std::error_code error;
        fs::remove(path,error);
        if (error) io("CONVERT_JOURNAL",error.message(),path);
    }

private:
    fs::path path;
    std::string identity;
    std::vector<std::unique_ptr<DirectoryLock>> locks;
    Json data;
    bool resumed=false;
};

void verify_identity(const Json &record) {
    for (const auto *field : {"inputs","resources"}) {
        for (const auto &entry : record.at(field)) {
            const auto path=fs::u8path(entry.at("canonical_source").get<std::string>());
            if (!fs::is_regular_file(destination_status(path)) || sha256_file(path)!=entry.at("sha256"))
                throw Failure(5,"recovery","CONVERT_CHANGED_INPUT","Recorded input changed after interruption",path);
        }
    }
}

bool recover(Json &entry, const Item &item, Json &record) {
    if (entry.is_null()) return false;
    const auto stage=entry.at("stage").get<std::string>();
    if (stage!="prepared" && stage!="reported")
        throw Failure(6,"recovery","CONVERT_JOURNAL","Unknown journal item stage");
    auto previous=entry.at("record");
    if (previous.at("key")!=item.key)
        throw Failure(6,"recovery","CONVERT_JOURNAL","Journal key differs from invocation");

    if (stage=="reported" && previous.at("status")=="failed") {
        record=std::move(previous);
        return true;
    }
    verify_identity(previous);
    if (!item.output) {
        if (stage=="prepared") return false;
        record=std::move(previous);
        return true;
    }
    const bool present=fs::is_regular_file(destination_status(*item.output));
    const auto existing=present ? Json(sha256_file(*item.output)) : Json(nullptr);
    if (existing==previous.at("output").at("bytes_sha256")) {
        previous["status"]="converted";
        record=std::move(previous);
        return true;
    }
    if (stage=="prepared" && existing==entry.at("prior_output")) return false;
    throw Failure(6,"recovery","CONVERT_CHANGED_OUTPUT","Recorded output differs after interruption",*item.output);
}

void item_failure(Json &record,const Failure &error) {
    record["status"]="failed";
    record["failure"]=error.record();
    record["checks"].push_back({{"gate",record["failure"]["stage"]},{"status","fail"},{"evidence",record["failure"]}});
}
}

std::string read_file(const fs::path &path, std::size_t limit, int exit) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    const auto size=input.tellg();
    if (size<0 || static_cast<std::uintmax_t>(size)>limit)
        throw Failure(exit,exit==2 ? "selection" : "output","CONVERT_INPUT",
                      "Cannot open file or file exceeds the permitted size",path);
    std::string contents(static_cast<std::size_t>(size),'\0');
    input.seekg(0);
    if (!input.read(contents.data(),static_cast<std::streamsize>(size)) ||
        input.peek()!=std::char_traits<char>::eof() || input.bad())
        throw Failure(exit,exit==2 ? "selection" : "output","CONVERT_INPUT","Cannot read the complete file",path);
    return contents;
}

fs::path destination(std::string_view text) {
    if (text.empty() || std::any_of(text.begin(),text.end(),[](unsigned char c) {return c<0x20||c==0x7f;}))
        throw Failure(2,"selection","CONVERT_USAGE","Invalid destination path");
    const auto path=fs::u8path(text);
    const auto filename=path.filename();
    if (!path.is_absolute() || filename.empty() || filename=="." || filename=="..")
        throw Failure(2,"selection","CONVERT_USAGE","Destination must be an absolute file path",path);
    std::error_code error;
    const auto parent=fs::weakly_canonical(path.parent_path(),error);
    if (error) io("CONVERT_OUTPUT",error.message(),path);
    return parent/filename;
}

void protect(const fs::path &destination,const std::vector<fs::path> &inputs) {
    const auto found=destination_status(destination);
    if (fs::exists(found) && !fs::is_regular_file(found))
        io("CONVERT_COLLISION","Destination is not a regular file",destination);
    for (const auto &path : inputs) {
        std::error_code canonical_error,equivalent_error;
        if (destination==fs::weakly_canonical(path,canonical_error) ||
            (fs::exists(found) && fs::equivalent(destination,path,equivalent_error)))
            io("CONVERT_INPUT_COLLISION","A source or asset cannot be a publication destination",destination);
    }
}

void publish(const fs::path &target,std::string_view bytes,bool replace) {
    protect(target,{});
    if (!replace && occupied(target)) io("CONVERT_COLLISION","Destination already exists",target);
    Temporary temp(target);
    temp.finish(bytes);
    std::error_code error;
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temp.path.c_str(),target.c_str(),flags))
        error=std::error_code(GetLastError(),std::system_category());
#else
    if (replace) fs::rename(temp.path,target,error);
    else fs::create_hard_link(temp.path,target,error);
#endif
    if (error) io("CONVERT_OUTPUT",error.message(),target);
}

int execute(const Plan &plan) {
    const auto converter=executable_identity();
    Journal journal(plan,converter.at("executable_sha256").get<std::string>());
    struct Row {
        Json record;
        std::optional<std::string> pending;
        Json prior_output;
    };
    std::vector<Row> rows;
    std::vector<fs::path> protected_paths;
    if (!plan.batch_path.empty()) protected_paths.push_back(plan.batch_path);
    for (const auto &item : plan.items) protected_paths.push_back(item.entry);

    for (std::size_t i=0;i<plan.items.size();++i) {
        const auto &item=plan.items[i];
        Row row{base_record(item,converter),{},{}};
        try {
            if (!recover(journal.at(i),item,row.record)) {
                if (item.output && occupied(*item.output)) {
                    if (!plan.force) io("CONVERT_COLLISION","Output already exists",*item.output);
                    row.prior_output=sha256_file(*item.output);
                }
                row.pending=evaluate(item,plan.manifest,row.record);
            }
        }
        catch (const Failure &error) { item_failure(row.record,error); }
        catch (const std::exception &error) {
            item_failure(row.record,Failure(7,"internal","CONVERT_INTERNAL",error.what(),item.entry));
        }
        for (const auto *field : {"inputs","resources"})
            for (const auto &entry : row.record.at(field))
                protected_paths.push_back(fs::u8path(entry.at("canonical_source").get<std::string>()));
        if (row.record["status"]=="failed" && row.record["failure"]["exit"]==5 &&
            row.record["failure"]["path"].is_string())
            protected_paths.push_back(fs::u8path(row.record["failure"]["path"].get<std::string>()));
        rows.push_back(std::move(row));
    }

    // Files consumed by any item are protected before this invocation can
    // replace a destination. An invalid manifest target cannot report to itself.
    try {
        protect(plan.manifest,protected_paths);
        protect(journal.filename(),protected_paths);
    }
    catch (const Failure &) {
        if (!journal.was_interrupted()) journal.close();
        throw;
    }

    auto results=Json::array();
    Json counts={{"requested",plan.items.size()},{"converted",0},{"validated_only",0},
                 {"parity_verified",0},{"failed",0},{"blocked",0}};
    int single_exit=0;
    for (std::size_t i=0;i<rows.size();++i) {
        const auto &item=plan.items[i];
        auto &row=rows[i];
        auto &record=row.record;
        auto &entry=journal.at(i);
        try {
            if (item.output) protect(*item.output,protected_paths);
            if (row.pending) {
                if (item.output) {
                    entry={{"stage","prepared"},{"record",record},{"prior_output",row.prior_output}};
                    journal.save();
                    publish(*item.output,*row.pending,plan.force);
                    record["status"]="converted";
                }
                else record["status"]="validated-only";
            }
        }
        catch (const Failure &error) { item_failure(record,error); }
        entry={{"stage","reported"},{"record",record}};
        journal.save();
        if (record["status"]=="converted") ++counts["converted"].get_ref<Json::number_integer_t &>();
        else if (record["status"]=="validated-only") ++counts["validated_only"].get_ref<Json::number_integer_t &>();
        else {
            ++counts["failed"].get_ref<Json::number_integer_t &>();
            single_exit=record.at("failure").at("exit").get<int>();
            std::cerr << record.at("failure").dump() << '\n';
        }
        results.push_back(std::move(record));
    }
    const auto exit=plan.batch_path.empty() ? single_exit : (single_exit==0 ? 0 : 8);
    const auto final=plan.batch_path.empty() ? results.at(0) :
        Json{{"record_version",1},{"items",results},{"counts",counts},{"exit",exit}};
    const auto bytes=final.dump(2)+'\n';
    if (!(journal.was_interrupted() && occupied(plan.manifest) &&
          read_file(plan.manifest,journal_limit,6)==bytes))
        publish(plan.manifest,bytes,plan.force);
    journal.close();
    return exit;
}

}
