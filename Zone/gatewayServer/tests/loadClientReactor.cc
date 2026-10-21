#include "loadClientReactor.h"
#include "loadClientSchedule.h"
#include "loadClientProtocol.h"
#include "aoiVerifier.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace std::chrono_literals;
constexpr size_t kIoBudget = 64 * 1024;
constexpr size_t kInputLimit = 4 * 1024 * 1024;
constexpr size_t kOutputLimit = 1024 * 1024;

std::string Encode(ClientKind kind, uint16_t module, uint16_t method, uint64_t id,
                   const std::string& body)
{
    ClientHeader h;
    h.kind = kind; h.module = module; h.method = method; h.requestId = id;
    Buffer b;
    EncodeClientFrame(b, h, body);
    return {b.peek(), b.readableBytes()};
}

enum class Stage { New, Connecting, Auth, Enter, Ready, Running, Drain, Hold, AoiFinal, Logout, Closed };

struct Pending
{
    Time sent;
    Time scheduled;
    bool measured = false;
    bool fullySent = false;
    int32_t sx = 0, sy = 0;
};

struct Output
{
    std::string bytes;
    size_t offset = 0;
    uint64_t businessId = 0;
};

struct Connection
{
    uint32_t index;
    int fd = -1;
    Stage stage = Stage::New;
    Buffer input;
    std::deque<Output> output;
    size_t outputBytes = 0;
    std::map<uint64_t, Pending> pending;
    Time expires = Time::max(), nextSend = Time::max(), writeProgress = Time::max();
    Time drainDeadline = Time::max(), aoiDeadline = Time::max();
    uint64_t requestId = 3, logoutId = 0, successfulMoves = 0;
    int32_t x = 0, y = 0, origin = 0;
    int direction = 1;
    bool measured = false, cutoff = false, buffered = false, eof = false;
    bool aoiEvidence = false;
    AoiVerifier verifier;
    explicit Connection(uint32_t i) : index(i) {}
    ~Connection() { if (fd >= 0) ::close(fd); }
};

struct Shared
{
    const LoadClientReactorOptions& options;
    LoadStats& stats;
    std::atomic<int64_t>& epoch;
    std::atomic<uint64_t>& successful;
    const volatile std::sig_atomic_t& stop;
    std::atomic<uint32_t> ready{0};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> busyEvidence{0};
    std::mutex files;
    Time preparationDeadline;
    bool verifyAoi = false;
    size_t aoiTotal = 0;
    size_t aoiObservers = 3;
    Shared(const LoadClientReactorOptions& o, LoadStats& s, std::atomic<int64_t>& e,
           std::atomic<uint64_t>& w, const volatile std::sig_atomic_t& stopped)
        : options(o), stats(s), epoch(e), successful(w), stop(stopped) {}
};

class Reactor
{
public:
    Reactor(Shared& shared, uint32_t shard, uint32_t count) : shared_(shared), o_(shared.options)
    {
        epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_ < 0) throw std::runtime_error("epoll_create1 failed");
        try
        {
            for (uint64_t i = shard; i < o_.connections; i += count)
                connections_.push_back(std::make_unique<Connection>(static_cast<uint32_t>(i)));
        }
        catch (...) { ::close(epoll_); epoll_ = -1; throw; }
    }
    ~Reactor() { if (epoll_ >= 0) ::close(epoll_); }

    void run()
    {
        try { loop(); }
        catch (const std::exception& e)
        {
            std::cerr << "load reactor: " << e.what() << '\n';
            shared_.stats.recordFailure(Result::TransportError);
            shared_.failed = true;
        }
        catch (...)
        {
            shared_.stats.recordFailure(Result::TransportError);
            shared_.failed = true;
        }
        for (auto& c : connections_)
            if (c->stage != Stage::Closed) close(*c, true);
    }

private:
    Shared& shared_;
    const LoadClientReactorOptions& o_;
    int epoll_ = -1;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::map<uint32_t, Connection*> byIndex_;
    LoadClientSchedule schedule_;
    LoadStats warmup_;
    size_t nextNew_ = 0, preparing_ = 0, closed_ = 0;
    bool announced_ = false;
    Time nextControl_ = Time::min();
    Time aoiAllReady_ = Time::max();
    size_t aoiReady_ = 0, aoiDone_ = 0;

    Time loadStart() const
    {
        return Time(std::chrono::milliseconds(shared_.epoch.load())) + std::chrono::seconds(o_.idleBeforeSec + o_.warmupSec);
    }
    void recordPhase(const Connection& c, const char* phase)
    {
        if (!o_.idleBeforeSec && !o_.holdAfterSec) return;
        std::lock_guard<std::mutex> lock(shared_.files);
        std::ofstream file(o_.output + ".phases.jsonl", std::ios::app);
        file << "{\"connection\":" << c.index << ",\"phase\":\"" << phase
             << "\",\"monotonicMs\":" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() << "}\n";
        if (!file) throw std::runtime_error("phase evidence write failed");
    }
    Time deadline() const { return loadStart() + std::chrono::seconds(o_.durationSec); }
    std::chrono::microseconds interval() const
    {
        return std::chrono::microseconds(std::max<uint64_t>(1, 1000000ull / o_.rpsPerConnection));
    }
    std::chrono::microseconds phase(const Connection& c) const
    {
        return o_.stagger ? std::chrono::microseconds(((o_.playerBase + c.index) * 7919) % interval().count())
                          : 250000us;
    }
    LoadStats& stats(bool measured) { return measured ? shared_.stats : warmup_; }
    uint64_t measuredPending(const Connection& c) const
    {
        uint64_t n = 0;
        for (const auto& p : c.pending) n += p.second.measured;
        return n;
    }
    void close(Connection& c, bool unresolved)
    {
        if (c.stage == Stage::Closed) return;
        if (c.stage == Stage::Logout) shared_.stats.recordLogout(false);
        if (c.stage == Stage::Connecting || c.stage == Stage::Auth || c.stage == Stage::Enter)
            --preparing_;
        if (unresolved)
        {
            auto n = measuredPending(c);
            for (const auto& p : c.pending)
            {
                stats(p.second.measured).recordUnresolved(1);
                stats(p.second.measured).recordInFlightDelta(-1);
            }
            if (c.cutoff) shared_.stats.recordDrainFinalUnfinished(n);
        }
        c.pending.clear();
        c.output.clear();
        c.outputBytes = 0;
        if (c.fd >= 0)
        {
            ::epoll_ctl(epoll_, EPOLL_CTL_DEL, c.fd, nullptr);
            ::close(c.fd); c.fd = -1;
        }
        c.stage = Stage::Closed;
        schedule_.erase(c.index);
        ++closed_;
    }
    void fail(Connection& c, Result why)
    {
        if (c.stage == Stage::Closed) return;
        // 准备失败使全局屏障失败；不伪造一次业务请求的 completed。
        if (shared_.epoch.load() <= 0 || c.stage == Stage::Connecting || c.stage == Stage::Auth || c.stage == Stage::Enter)
            shared_.failed = true;
        if (c.pending.empty()) shared_.stats.recordFailure(why);
        else
        {
            bool first = true;
            bool measuredFailure = false;
            for (const auto& entry : c.pending)
            {
                measuredFailure |= entry.second.measured;
                auto& s = stats(entry.second.measured);
                if (first) s.recordResult(why);
                else s.recordUnresolved(1);
                s.recordInFlightDelta(-1);
                first = false;
            }
            c.pending.clear();
            // 预热阶段的终止故障也必须让执行器返回失败，但不能污染正式计数。
            if (!measuredFailure) shared_.failed = true;
        }
        close(c, false);
    }
    void interest(Connection& c, bool add = false)
    {
        if (c.fd < 0) return;
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        if (c.stage == Stage::Connecting || !c.output.empty()) event.events |= EPOLLOUT;
        event.data.ptr = &c;
        if (::epoll_ctl(epoll_, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, c.fd, &event) != 0)
            fail(c, Result::TransportError);
    }
    bool enqueue(Connection& c, std::string bytes, uint64_t id = 0)
    {
        if (bytes.size() > kOutputLimit - std::min(c.outputBytes, kOutputLimit))
        { fail(c, Result::TransportError); return false; }
        if (c.output.empty()) c.writeProgress = Clock::now();
        c.outputBytes += bytes.size();
        c.output.push_back({std::move(bytes), 0, id});
        interest(c);
        return c.stage != Stage::Closed;
    }
    void authenticatedSocket(Connection& c)
    {
        c.stage = Stage::Auth;
        c.expires = Clock::now() + 3s;
        enqueue(c, Encode(ClientKind::kControl, 1, ClientSysMethod::kAuth, 1,
                           MakeAuthBody(o_.playerBase + c.index, o_.ticket)));
    }
    void connect(Connection& c)
    {
        c.stage = Stage::Connecting;
        ++preparing_;
        c.expires = Clock::now() + 3s;
        c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (c.fd < 0) { fail(c, Result::TransportError); return; }
        int one = 1;
        ::setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(o_.port);
        if (::inet_pton(AF_INET, o_.host.c_str(), &address.sin_addr) != 1)
        { fail(c, Result::TransportError); return; }
        int rc = ::connect(c.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (rc < 0 && errno != EINPROGRESS) { fail(c, Result::TransportError); return; }
        interest(c, true);
        if (rc == 0 && c.stage != Stage::Closed) authenticatedSocket(c);
        reschedule(c);
    }
    void prepared(Connection& c)
    {
        --preparing_;
        c.stage = Stage::Ready;
        recordPhase(c, "idle");
        c.expires = shared_.preparationDeadline;
        c.origin = c.x;
        shared_.ready.fetch_add(1);
    }
    void write(Connection& c)
    {
        size_t budget = kIoBudget;
        while (!c.output.empty() && budget)
        {
            auto& out = c.output.front();
            auto n = ::send(c.fd, out.bytes.data() + out.offset,
                            std::min(budget, out.bytes.size() - out.offset), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) { fail(c, Result::TransportError); return; }
            budget -= static_cast<size_t>(n);
            out.offset += static_cast<size_t>(n);
            c.outputBytes -= static_cast<size_t>(n);
            c.writeProgress = Clock::now();
            if (out.offset != out.bytes.size()) continue;
            if (out.businessId)
            {
                auto p = c.pending.find(out.businessId);
                if (p != c.pending.end())
                {
                    p->second.fullySent = true;
                    stats(p->second.measured).recordSend(static_cast<uint64_t>(std::max<int64_t>(0,
                        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - p->second.scheduled).count())));
                }
            }
            c.output.pop_front();
        }
        interest(c);
    }
    void acceptPush(Connection& c, const ClientHeader& h, const std::string& body)
    {
        if (o_.scene) ApplySceneStatePush(h, body, c.x, c.y);
        if (!shared_.verifyAoi || c.index != 0 || h.module != 100) return;
        if (h.method == 7)
        {
            gs::SCPushViewDelta delta;
            if (delta.ParseFromString(body)) c.verifier.delta(delta);
        }
        else if (h.method == 8)
        {
            gs::SCPushViewSnapshotPart part;
            if (part.ParseFromString(body)) c.verifier.snapshot(part);
        }
    }
    void response(Connection& c, const ClientHeader& h, const std::string& body)
    {
        if (h.kind == ClientKind::kPush) { acceptPush(c, h, body); return; }
        if (c.stage == Stage::Auth)
        {
            uint64_t player = 0, session = 0; uint32_t epoch = 0, zone = 0; uint8_t service = 0;
            if (h.kind != ClientKind::kControl || h.module != 1 || h.method != 2 || h.requestId != 1 ||
                !ParseAuthAckBody(body, player, session, epoch, zone, service) || player != o_.playerBase + c.index)
            { fail(c, Result::TransportError); return; }
            if (!o_.scene) { prepared(c); return; }
            c.stage = Stage::Enter; c.expires = Clock::now() + 5s;
            enqueue(c, Encode(ClientKind::kRequest, 3, 4, 2, gs::EnterSceneReq{}.SerializeAsString()));
            return;
        }
        if (c.stage == Stage::Enter)
        {
            if (!MatchesLoadRequestResponse(h, 2, 3, 4) ||
                ClassifyLoadResponse(h, body, 3, 4) != LoadResponseKind::Success ||
                !AdvanceEnterScenePosition(body, c.x, c.y)) { fail(c, Result::TransportError); return; }
            prepared(c); return;
        }
        if (c.stage == Stage::Logout)
        {
            bool ok = IsSuccessfulLogoutResponse(h, c.logoutId, body);
            shared_.stats.recordLogout(ok);
            if (ok)
            {
                std::lock_guard<std::mutex> lock(shared_.files);
                std::ofstream positions(o_.output + ".players.jsonl", std::ios::app);
                if (!(positions << "{\"playerId\":" << o_.playerBase + c.index << ",\"x\":" << c.x
                    << ",\"y\":" << c.y << ",\"successfulMoves\":" << c.successfulMoves << "}\n"))
                    shared_.stats.recordFailure(Result::TransportError);
            }
            c.stage = Stage::Drain; // close 不重复记录 logout_failed。
            close(c, false); return;
        }
        auto p = c.pending.find(h.requestId);
        if (p == c.pending.end()) { fail(c, Result::UnknownResponse); return; }
        auto pending = p->second;
        auto& s = stats(pending.measured);
        uint16_t module = o_.ping ? 1 : (o_.scene ? 3 : o_.module);
        uint16_t method = o_.ping ? ClientSysMethod::kPong : (o_.scene ? 2 : o_.method);
        const bool matches = o_.ping
            ? h.requestId == p->first && h.module == 1 && h.method == ClientSysMethod::kPong &&
                (h.kind == ClientKind::kControl || h.kind == ClientKind::kError)
            : MatchesLoadRequestResponse(h, p->first, module, method);
        if (!matches) { fail(c, Result::UnknownResponse); return; }
        auto classified = o_.ping ? (h.kind == ClientKind::kError ? LoadResponseKind::GatewayReject : LoadResponseKind::Success)
                                  : ClassifyLoadResponse(h, body, module, method);
        if (classified == LoadResponseKind::Mismatch) { fail(c, Result::DecodeError); return; }
        const auto now = Clock::now();
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(now - pending.sent).count();
        if (classified == LoadResponseKind::Success)
        {
            s.recordResult(o_.ping ? Result::ControlResponse : Result::BusinessResponse, latency, body.size());
            if (pending.measured && now < deadline()) ++shared_.successful;
            if (o_.scene)
            {
                if (pending.sx == c.x && pending.sy == c.y) AdvanceWalkPosition(body, c.x, c.y);
                ++c.successfulMoves;
            }
        }
        else if (classified == LoadResponseKind::GatewayReject) s.recordResult(Result::GatewayReject);
        else
        {
            gs::RetTip tip; gs::WalkAck ack;
            std::string reason = "unclassified";
            if (IsRetTipBody(body) && tip.ParseFromString(body)) reason = std::to_string(tip.code()) + ":" + tip.text();
            else if (o_.scene && ack.ParseFromString(body) && ack.code()) reason = std::to_string(ack.code()) + ":WalkAck";
            s.recordBusinessError(reason); s.recordResult(Result::BusinessError);
            if (std::getenv("SCENE_BUSY_DIAGNOSTICS") && reason == "1011:server busy")
            {
                auto count = shared_.busyEvidence.fetch_add(1);
                if (count < 4096) std::fprintf(stderr, "BUSY_EVIDENCE player=%llu request=%llu receivedUs=%llu measured=%d\n",
                    static_cast<unsigned long long>(o_.playerBase + c.index), static_cast<unsigned long long>(h.requestId),
                    static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()), pending.measured);
                else if (count == 4096) std::fprintf(stderr, "BUSY_EVIDENCE_DROPPED\n");
            }
        }
        s.recordInFlightDelta(-1);
        c.pending.erase(p);
    }
    void read(Connection& c)
    {
        size_t bytes = 0, frames = 0;
        c.buffered = false;
        for (;;)
        {
            while (frames < 64)
            {
                ClientHeader h; std::string body;
                auto status = DecodeClientFrame(c.input, h, body);
                if (status == DecodeStatus::kNeedMore) break;
                if (status == DecodeStatus::kError) { fail(c, Result::DecodeError); return; }
                ++frames;
                response(c, h, body);
                if (c.stage == Stage::Closed) return;
            }
            if (frames == 64)
            {
                // 用户态仍有完整帧时显式安排下一轮，不能只依赖 fd 再次可读。
                c.buffered = c.input.readableBytes() != 0;
                return;
            }
            if (c.eof) { fail(c, Result::TransportError); return; }
            if (bytes >= kIoBudget) return;
            char buffer[16384];
            auto n = ::recv(c.fd, buffer, std::min(sizeof(buffer), kIoBudget - bytes), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (n < 0) { fail(c, Result::TransportError); return; }
            if (n == 0) { c.eof = true; continue; }
            bytes += static_cast<size_t>(n);
            if (c.input.readableBytes() + static_cast<size_t>(n) > kInputLimit)
            { fail(c, Result::DecodeError); return; }
            c.input.append(buffer, static_cast<size_t>(n));
        }
    }
    void beginDrain(Connection& c, Time now)
    {
        recordPhase(c, "drain");
        c.stage = Stage::Drain;
        c.cutoff = true;
        shared_.stats.recordCutoffUnfinished(measuredPending(c));
        c.drainDeadline = std::min(deadline() + std::chrono::seconds(o_.drainSec),
                                  now + std::chrono::seconds(o_.drainSec));
    }
    std::filesystem::path aoiDirectory() const
    { return std::filesystem::path(o_.barrierDir).parent_path() / "aoi-final"; }
    void finishLoad(Connection& c, Time now)
    {
        if (!o_.scene || shared_.stop) { close(c, false); return; }
        if (shared_.verifyAoi)
        {
            auto dir = aoiDirectory();
            std::filesystem::create_directories(dir);
            std::ofstream ready(dir / (std::to_string(o_.playerBase + c.index) + ".ready"));
            if (!(ready << "{\"playerId\":" << o_.playerBase + c.index << ",\"x\":" << c.x << ",\"y\":" << c.y << "}\n"))
            { fail(c, Result::TransportError); return; }
            c.stage = Stage::AoiFinal; c.aoiDeadline = now + 15s;
        }
        else logout(c);
    }
    void logout(Connection& c)
    {
        recordPhase(c, "logout");
        c.stage = Stage::Logout; c.expires = Clock::now() + 5s; c.logoutId = c.requestId++;
        enqueue(c, Encode(ClientKind::kRequest, 3, 5, c.logoutId, gs::LogoutReq{}.SerializeAsString()));
    }
    void aoiFinal(Connection& c, Time now)
    {
        if (shared_.stop) { close(c, false); return; }
        bool settled = aoiAllReady_ != Time::max() && now >= aoiAllReady_ + 1s;
        if (!c.aoiEvidence && (settled || now >= c.aoiDeadline))
        {
            c.aoiEvidence = true;
            if (c.index == 0)
            {
                auto dir = aoiDirectory(); auto player = o_.playerBase + c.index;
                std::ofstream evidence(dir / (std::to_string(player) + ".view.json"));
                evidence << "{\"playerId\":" << player << ",\"complete\":" << (c.verifier.complete() ? "true" : "false")
                    << ",\"errors\":" << c.verifier.errors() << ",\"viewSeq\":" << c.verifier.sequence() << ",\"entities\":[";
                bool first = true;
                for (const auto& item : c.verifier.entities())
                {
                    const auto& e = item.second;
                    if (!first) evidence << ',';
                    first = false;
                    evidence << "{\"entityId\":" << item.first << ",\"businessId\":" << e.business_id()
                        << ",\"x\":" << e.x() << ",\"y\":" << e.y() << ",\"generation\":" << e.entity_generation()
                        << ",\"spatialVersion\":" << e.spatial_version() << '}';
                }
                evidence << "]}\n";
                evidence.close();
                if (!evidence) { fail(c, Result::TransportError); return; }
                std::ofstream done(dir / (std::to_string(player) + ".done"));
                if (!done) { fail(c, Result::TransportError); return; }
            }
        }
        if (c.aoiEvidence && (aoiDone_ >= shared_.aoiObservers || now >= c.aoiDeadline)) logout(c);
    }
    void tick(Connection& c, Time now)
    {
        if (c.stage == Stage::Closed) return;
        if (!c.output.empty() && now >= c.writeProgress + 2s) { fail(c, Result::TransportError); return; }
        if ((c.stage == Stage::Connecting || c.stage == Stage::Auth || c.stage == Stage::Enter || c.stage == Stage::Logout) && now >= c.expires)
        { fail(c, Result::Timeout); return; }
        if (c.stage == Stage::Ready && shared_.epoch.load() > 0)
        {
            if (shared_.stop) { close(c, true); return; }
            if (now < Time(std::chrono::milliseconds(shared_.epoch.load())) + std::chrono::seconds(o_.idleBeforeSec)) return;
            c.stage = Stage::Running;
            recordPhase(c, "load");
            c.requestId = o_.scene ? 3 : 2;
            c.nextSend = Time(std::chrono::milliseconds(shared_.epoch.load())) + std::chrono::seconds(o_.idleBeforeSec) + phase(c);
        }
        if (shared_.stop && (c.stage == Stage::Logout || c.stage == Stage::AoiFinal))
        { close(c, true); return; }
        if (c.stage == Stage::Running)
        {
            if (!c.measured && now >= loadStart())
            { c.measured = true; c.nextSend = loadStart() + phase(c); }
            if (shared_.stop || now >= deadline()) beginDrain(c, now);
            else
            {
                for (int budget = 0; budget < 16 && c.nextSend <= now && !(o_.scene && !c.pending.empty()); ++budget)
                {
                    uint64_t id = c.requestId++;
                    std::string body = o_.body;
                    if (o_.ping) body.clear();
                    else if (o_.scene)
                    {
                        gs::WalkReq req;
                        req.set_sx(c.x); req.set_sy(c.y);
                        req.set_dx(NextLoadWalkX(c.x, c.origin, c.direction)); req.set_dy(c.y);
                        body = req.SerializeAsString();
                    }
                    auto bytes = Encode(o_.ping ? ClientKind::kControl : ClientKind::kRequest,
                        o_.ping ? 1 : (o_.scene ? 3 : o_.module),
                        o_.ping ? ClientSysMethod::kPing : (o_.scene ? 2 : o_.method), id, body);
                    auto& s = stats(c.measured);
                    s.recordAttempt(bytes.size()); s.recordInFlightDelta(1);
                    c.pending.emplace(id, Pending{now, c.nextSend, c.measured, false, c.x, c.y});
                    c.nextSend += interval();
                    if (!enqueue(c, std::move(bytes), id)) return;
                }
            }
        }
        // 最老请求由请求号排序，所有发送时刻单调；到期后关闭连接，拒绝迟到响应串账。
        if (!c.pending.empty() && now >= c.pending.begin()->second.sent + 5s)
        { fail(c, Result::Timeout); return; }
        if (c.stage == Stage::Drain)
        {
            if (c.pending.empty() && c.output.empty())
            {
                if (o_.holdAfterSec && !shared_.stop) {
                    recordPhase(c, "hold");
                    c.stage = Stage::Hold;
                    c.drainDeadline = now + std::chrono::seconds(o_.holdAfterSec);
                } else finishLoad(c, now);
            }
            else if (now >= c.drainDeadline)
            {
                auto n = measuredPending(c);
                shared_.stats.recordDrainUnfinished(n);
                close(c, true);
            }
        }
        if (c.stage == Stage::Hold && (shared_.stop || now >= c.drainDeadline)) finishLoad(c, now);
        if (c.stage == Stage::AoiFinal) aoiFinal(c, now);
        if (c.buffered && c.stage != Stage::Closed) read(c);
        if (!c.output.empty() && c.stage != Stage::Connecting && c.stage != Stage::Closed) write(c);
    }
    void reschedule(Connection& c)
    {
        if (c.stage == Stage::Closed) return;
        Time next = Time::max();
        if (!c.output.empty()) next = c.writeProgress + 2s;
        if (c.stage == Stage::Connecting || c.stage == Stage::Auth || c.stage == Stage::Enter || c.stage == Stage::Logout)
            next = std::min(next, c.expires);
        if (c.stage == Stage::Ready) next = std::min(next, Clock::now() + 20ms);
        if (c.stage == Stage::Running)
        {
            next = std::min(next, deadline());
            if (!c.measured) next = std::min(next, loadStart());
            if (!(o_.scene && !c.pending.empty())) next = std::min(next, c.nextSend);
        }
        if (!c.pending.empty()) next = std::min(next, c.pending.begin()->second.sent + 5s);
        if (c.stage == Stage::Drain) next = std::min(next, c.pending.empty() && c.output.empty() ? Clock::now() : c.drainDeadline);
        if (c.stage == Stage::Hold) next = std::min(next, c.drainDeadline);
        if (c.stage == Stage::AoiFinal) next = std::min(next, Clock::now() + 20ms);
        if (c.buffered) next = Clock::now();
        schedule_.update(c.index, next);
    }
    void control(Time now)
    {
        if (now < nextControl_) return;
        nextControl_ = now + 20ms;
        if (shared_.epoch.load() <= 0)
        {
            if (shared_.stop || now >= shared_.preparationDeadline)
            { shared_.failed = true; return; }
            if (!o_.barrierDir.empty() && std::filesystem::exists(std::filesystem::path(o_.barrierDir) / "ABORT"))
            { shared_.failed = true; return; }
            if (shared_.ready.load() == o_.connections)
            {
                if (!announced_)
                {
                    std::lock_guard<std::mutex> lock(shared_.files);
                    for (const auto& c : connections_)
                        if (!o_.barrierDir.empty())
                        {
                            std::ofstream ready(std::filesystem::path(o_.barrierDir) /
                                (std::filesystem::path(o_.output).filename().string() + "." + std::to_string(c->index) + ".ready"));
                            if (!(ready << "ready\n")) throw std::runtime_error("cannot write ready file");
                        }
                    announced_ = true;
                }
                if (o_.barrierDir.empty())
                {
                    int64_t expected = 0;
                    shared_.epoch.compare_exchange_strong(expected,
                        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
                }
                else
                {
                    std::ifstream start(std::filesystem::path(o_.barrierDir) / "START");
                    if (start)
                    {
                        int64_t epoch = 0;
                        // 兼容空 START 文件；负值、非法内容不能被当成立即开始。
                        if (!(start >> epoch) && !start.eof()) { shared_.failed = true; return; }
                        if (!epoch) epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        // 只接受准备期限内的未来屏障，防止畸形时间戳导致无限等待。
                        if (epoch < nowMs - 60000 || epoch >
                            std::chrono::duration_cast<std::chrono::milliseconds>(shared_.preparationDeadline.time_since_epoch()).count())
                        { shared_.failed = true; return; }
                        int64_t expected = 0; shared_.epoch.compare_exchange_strong(expected, epoch);
                    }
                }
            }
        }
        if (shared_.verifyAoi && std::filesystem::exists(aoiDirectory()))
        {
            aoiReady_ = aoiDone_ = 0;
            for (const auto& entry : std::filesystem::directory_iterator(aoiDirectory()))
            {
                if (entry.path().extension() == ".ready") ++aoiReady_;
                if (entry.path().extension() == ".done") ++aoiDone_;
            }
            if (aoiReady_ == shared_.aoiTotal && aoiAllReady_ == Time::max()) aoiAllReady_ = now;
        }
        if (shared_.stop)
            for (auto& c : connections_)
                if (c->stage == Stage::Hold || c->stage == Stage::Running || c->stage == Stage::Ready || c->stage == Stage::AoiFinal)
                { tick(*c, now); reschedule(*c); }
    }
    void loop()
    {
        for (auto& c : connections_) byIndex_[c->index] = c.get();
        epoll_event events[256];
        while (closed_ < connections_.size())
        {
            if (shared_.failed.load()) break;
            while (nextNew_ < connections_.size() && preparing_ < 64 && !shared_.failed.load())
                connect(*connections_[nextNew_++]);
            auto now = Clock::now();
            control(now);
            if (shared_.failed.load()) break;
            uint32_t id = 0;
            // 一个到期连接每轮至多处理一次，追赶计划不能阻塞其他连接和 fd 事件。
            size_t dueBudget = connections_.size();
            std::vector<Connection*> due;
            while (dueBudget-- && schedule_.pop(now, id)) due.push_back(byIndex_.at(id));
            for (auto* c : due) { tick(*c, Clock::now()); reschedule(*c); }
            if (closed_ == connections_.size()) break;
            auto timeout = schedule_.waitMs(Clock::now());
            timeout = std::min(timeout, static_cast<int>(std::max<int64_t>(0,
                std::chrono::duration_cast<std::chrono::milliseconds>(nextControl_ - Clock::now()).count())));
            int count = ::epoll_wait(epoll_, events, 256, timeout);
            if (count < 0)
            {
                if (errno == EINTR) continue;
                throw std::runtime_error("epoll_wait failed");
            }
            for (int i = 0; i < count; ++i)
            {
                auto& c = *static_cast<Connection*>(events[i].data.ptr);
                if (c.stage == Stage::Closed) continue;
                if (c.stage == Stage::Connecting)
                {
                    int error = 0; socklen_t len = sizeof(error);
                    if (::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error)
                    { fail(c, Result::TransportError); continue; }
                    authenticatedSocket(c);
                }
                if (c.stage == Stage::Closed) continue;
                if (events[i].events & EPOLLOUT) write(c);
                if (c.stage == Stage::Closed) continue;
                if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) read(c);
                // 已经消耗本轮帧预算时，留到调度器下一轮，避免同一 fd 连续吞掉预算。
                if (c.stage != Stage::Closed)
                {
                    if (!c.buffered) tick(c, Clock::now());
                    reschedule(c);
                }
            }
        }
    }
};
} // namespace

bool RunLoadClientReactors(const LoadClientReactorOptions& options, LoadStats& stats,
                          std::atomic<int64_t>& startEpochMs, std::atomic<uint64_t>& windowSuccessful,
                          const volatile std::sig_atomic_t& stopRequested)
{
    const char* verify = std::getenv("AOI_VERIFY_TOTAL");
    Shared shared{options, stats, startEpochMs, windowSuccessful, stopRequested};
    shared.preparationDeadline = Clock::now() + std::chrono::seconds(options.preparationTimeoutSec);
    shared.verifyAoi = options.scene && verify;
    shared.aoiTotal = verify ? std::strtoull(verify, nullptr, 10) : 0;
    // 保留旧三网关 AOI 工具的约定：每进程采样 index=0，共三个观察者。
    std::vector<std::unique_ptr<Reactor>> reactors;
    std::vector<std::thread> workers;
    try
    {
        uint32_t count = std::min(options.ioThreads, options.connections);
        for (uint32_t i = 0; i < count; ++i) reactors.push_back(std::make_unique<Reactor>(shared, i, count));
        for (auto& reactor : reactors) workers.emplace_back([ptr = reactor.get()] { ptr->run(); });
    }
    catch (const std::exception& e)
    {
        std::cerr << "cannot start load reactors: " << e.what() << '\n';
        shared.failed = true;
        stats.recordFailure(Result::TransportError);
    }
    for (auto& worker : workers) worker.join();
    return !shared.failed.load();
}