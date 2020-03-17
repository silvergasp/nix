#include "download.hh"
#include "fetchers/cache.hh"
#include "fetchers/fetchers.hh"
#include "fetchers/parse.hh"
#include "fetchers/regex.hh"
#include "globals.hh"
#include "store-api.hh"

#include <nlohmann/json.hpp>

namespace nix::fetchers {

std::regex ownerRegex("[a-zA-Z][a-zA-Z0-9_-]*", std::regex::ECMAScript);
std::regex repoRegex("[a-zA-Z][a-zA-Z0-9_-]*", std::regex::ECMAScript);

struct GitHubInput : Input
{
    std::string owner;
    std::string repo;
    std::optional<std::string> ref;
    std::optional<Hash> rev;

    std::string type() const override { return "github"; }

    bool operator ==(const Input & other) const override
    {
        auto other2 = dynamic_cast<const GitHubInput *>(&other);
        return
            other2
            && owner == other2->owner
            && repo == other2->repo
            && rev == other2->rev
            && ref == other2->ref;
    }

    bool isImmutable() const override
    {
        return (bool) rev;
    }

    std::optional<std::string> getRef() const override { return ref; }

    std::optional<Hash> getRev() const override { return rev; }

    std::string to_string() const override
    {
        auto s = fmt("github:%s/%s", owner, repo);
        assert(!(ref && rev));
        if (ref) s += "/" + *ref;
        if (rev) s += "/" + rev->to_string(Base16, false);
        return s;
    }

    Attrs toAttrsInternal() const override
    {
        Attrs attrs;
        attrs.emplace("owner", owner);
        attrs.emplace("repo", repo);
        if (ref)
            attrs.emplace("ref", *ref);
        if (rev)
            attrs.emplace("rev", rev->gitRev());
        return attrs;
    }

    void clone(const Path & destDir) const override
    {
        std::shared_ptr<const Input> input = inputFromURL(fmt("git+ssh://git@github.com/%s/%s.git", owner, repo));
        input = input->applyOverrides(ref.value_or("master"), rev);
        input->clone(destDir);
    }

    std::pair<Tree, std::shared_ptr<const Input>> fetchTreeInternal(nix::ref<Store> store) const override
    {
        auto rev = this->rev;
        auto ref = this->ref.value_or("master");

        Attrs mutableAttrs({
            {"type", "github"},
            {"owner", owner},
            {"repo", repo},
            {"ref", ref},
        });

        if (!rev) {
            if (auto res = getCache()->lookup(store, mutableAttrs)) {
                auto input = std::make_shared<GitHubInput>(*this);
                input->ref = {};
                input->rev = Hash(getStrAttr(res->first, "rev"), htSHA1);
                return {
                    Tree{
                        .actualPath = store->toRealPath(res->second),
                        .storePath = std::move(res->second),
                        .info = TreeInfo {
                            .lastModified = getIntAttr(res->first, "lastModified"),
                        },
                    },
                    input
                };
            }
        }

        if (!rev) {
            auto url = fmt("https://api.github.com/repos/%s/%s/commits/%s",
                owner, repo, ref);
            CachedDownloadRequest request(url);
            request.ttl = rev ? 1000000000 : settings.tarballTtl;
            auto result = getDownloader()->downloadCached(store, request);
            auto json = nlohmann::json::parse(readFile(result.path));
            rev = Hash(json["sha"], htSHA1);
            debug("HEAD revision for '%s' is %s", url, rev->gitRev());
        }

        auto input = std::make_shared<GitHubInput>(*this);
        input->ref = {};
        input->rev = *rev;

        Attrs immutableAttrs({
            {"type", "git-tarball"},
            {"rev", rev->gitRev()},
        });

        if (auto res = getCache()->lookup(store, immutableAttrs)) {
            return {
                Tree{
                    .actualPath = store->toRealPath(res->second),
                    .storePath = std::move(res->second),
                    .info = TreeInfo {
                        .lastModified = getIntAttr(res->first, "lastModified"),
                    },
                },
                input
            };
        }

        // FIXME: use regular /archive URLs instead? api.github.com
        // might have stricter rate limits.

        auto url = fmt("https://api.github.com/repos/%s/%s/tarball/%s",
            owner, repo, rev->to_string(Base16, false));

        std::string accessToken = settings.githubAccessToken.get();
        if (accessToken != "")
            url += "?access_token=" + accessToken;

        CachedDownloadRequest request(url);
        request.unpack = true;
        request.name = "source";
        request.ttl = 1000000000;
        request.getLastModified = true;
        auto dresult = getDownloader()->downloadCached(store, request);

        assert(dresult.lastModified);

        Tree result{
            .actualPath = dresult.path,
            .storePath = store->parseStorePath(dresult.storePath),
            .info = TreeInfo {
                .lastModified = *dresult.lastModified,
            },
        };

        Attrs infoAttrs({
            {"rev", rev->gitRev()},
            {"lastModified", *result.info.lastModified}
        });

        if (!this->rev)
            getCache()->add(
                store,
                mutableAttrs,
                infoAttrs,
                result.storePath,
                false);

        getCache()->add(
            store,
            immutableAttrs,
            infoAttrs,
            result.storePath,
            true);

        return {std::move(result), input};
    }

    std::shared_ptr<const Input> applyOverrides(
        std::optional<std::string> ref,
        std::optional<Hash> rev) const override
    {
        if (!ref && !rev) return shared_from_this();

        auto res = std::make_shared<GitHubInput>(*this);

        if (ref) res->ref = ref;
        if (rev) res->rev = rev;

        return res;
    }
};

struct GitHubInputScheme : InputScheme
{
    std::unique_ptr<Input> inputFromURL(const ParsedURL & url) override
    {
        if (url.scheme != "github") return nullptr;

        auto path = tokenizeString<std::vector<std::string>>(url.path, "/");
        auto input = std::make_unique<GitHubInput>();

        if (path.size() == 2) {
        } else if (path.size() == 3) {
            if (std::regex_match(path[2], revRegex))
                input->rev = Hash(path[2], htSHA1);
            else if (std::regex_match(path[2], refRegex))
                input->ref = path[2];
            else
                throw BadURL("in GitHub URL '%s', '%s' is not a commit hash or branch/tag name", url.url, path[2]);
        } else
            throw BadURL("GitHub URL '%s' is invalid", url.url);

        for (auto &[name, value] : url.query) {
            if (name == "rev") {
                if (input->rev)
                    throw BadURL("GitHub URL '%s' contains multiple commit hashes", url.url);
                input->rev = Hash(value, htSHA1);
            }
            else if (name == "ref") {
                if (!std::regex_match(value, refRegex))
                    throw BadURL("GitHub URL '%s' contains an invalid branch/tag name", url.url);
                if (input->ref)
                    throw BadURL("GitHub URL '%s' contains multiple branch/tag names", url.url);
                input->ref = value;
            }
        }

        if (input->ref && input->rev)
            throw BadURL("GitHub URL '%s' contains both a commit hash and a branch/tag name", url.url);

        input->owner = path[0];
        input->repo = path[1];

        return input;
    }

    std::unique_ptr<Input> inputFromAttrs(const Attrs & attrs) override
    {
        if (maybeGetStrAttr(attrs, "type") != "github") return {};

        for (auto & [name, value] : attrs)
            if (name != "type" && name != "owner" && name != "repo" && name != "ref" && name != "rev")
                throw Error("unsupported GitHub input attribute '%s'", name);

        auto input = std::make_unique<GitHubInput>();
        input->owner = getStrAttr(attrs, "owner");
        input->repo = getStrAttr(attrs, "repo");
        input->ref = maybeGetStrAttr(attrs, "ref");
        if (auto rev = maybeGetStrAttr(attrs, "rev"))
            input->rev = Hash(*rev, htSHA1);
        return input;
    }
};

static auto r1 = OnStartup([] { registerInputScheme(std::make_unique<GitHubInputScheme>()); });

}
