#pragma once

#include "types.hh"
#include "fetchers/fetchers.hh"

namespace nix::fetchers {

struct Cache
{
    virtual void add(
        ref<Store> store,
        const Attrs & inAttrs,
        const Attrs & infoAttrs,
        const StorePath & storePath,
        bool immutable) = 0;

    virtual std::optional<std::pair<Attrs, StorePath>> lookup(
        ref<Store> store,
        const Attrs & inAttrs) = 0;
};

ref<Cache> getCache();

}
