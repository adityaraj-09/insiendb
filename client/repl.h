#pragma once
#include "catalog.h"
#include "storage.h"
#include <cstdint>
#include <string>

void runRepl(Storage& storage, Catalog& catalog);
void runRemoteRepl(const std::string& host, uint16_t port);
