// Included inside the isolated worker namespace; shares its allocation injector.
void schemaDdlAllocationFaultCase(const std::string &mode, const std::filesystem::path &path, size_t fault) {
    RelocatableStorageBackendOptions options;
    options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    const bool vector = mode == "schema-vector-allocation-fault";
    const bool constraint = mode == "schema-constraint-allocation-fault";
    const bool drop = mode == "schema-drop-allocation-fault";
    const std::vector<LogicalGraphNodeIndexCreateRequest> requests{
        {std::string(1000U, 'a'), "FaultLabel", {"first"}},
        {std::string(1000U, 'b'), "FaultLabel", {"second"}}};
    if (drop) (void)backend->createLogicalNodeIndexes(requests);
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    const auto mutate = [&] {
        if (vector) (void)backend->createLogicalNodeVectorIndex({requests[0].name, "FaultLabel", "embedding", 2U, "l2"});
        else if (constraint) (void)backend->createLogicalConstraint({requests[0].name,
            LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_UNIQUE, "FaultLabel", {"first"}});
        else if (drop) (void)backend->dropLogicalIndexByName(requests[0].name);
        else (void)backend->createLogicalNodeIndexes(requests);
    };
    faultTriggered.store(false); const auto before = allocationAttempts.load();
    faultRemaining.store(fault); bool threw = false;
    try { mutate(); } catch (const std::exception &) { threw = true; }
    faultRemaining.store((std::numeric_limits<size_t>::max)());
    const auto attempts = allocationAttempts.load() - before;
    const bool triggered = faultTriggered.load(), indeterminate = backend->hasIndeterminateWriteOutcome();
    if (!triggered && threw) throw std::runtime_error("normal schema DDL probe failed");
    backend->close(); backend->open();
    bool published = false;
    if (constraint) {
        const auto actual = backend->listLogicalConstraints(8U);
        if (actual.size() > 1U || (!actual.empty() && actual[0].name != requests[0].name))
            throw std::runtime_error("schema constraint fault recovered an invalid definition");
        published = actual.size() == 1U;
    } else {
        const auto actual = backend->listLogicalIndexes(8U);
        if (drop) {
            if (actual.size() != 1U && actual.size() != 2U) throw std::runtime_error("schema drop fault lost unrelated index");
            if (std::none_of(actual.begin(), actual.end(), [&](const auto &entry) { return entry.name == requests[1].name; }))
                throw std::runtime_error("schema drop fault removed unrelated index");
            published = actual.size() == 1U;
        } else {
            const size_t expected = vector ? 1U : requests.size();
            if (!actual.empty() && actual.size() != expected) throw std::runtime_error("schema batch fault published a partial batch");
            for (size_t i = 0U; i < actual.size(); ++i)
                if (std::none_of(requests.begin(), requests.end(), [&](const auto &entry) { return entry.name == actual[i].name; }))
                    throw std::runtime_error("schema DDL fault changed definition names");
            published = actual.size() == expected;
        }
    }
    if (!threw && !published) throw std::runtime_error("successful schema DDL was not durable");
    if (threw && published && !indeterminate) throw std::runtime_error("definite failed schema DDL nevertheless published");
    if (!published && backend->currentNativeLogicalGraphMutationFence() != fence)
        throw std::runtime_error("failed schema DDL changed logical graph generation");
    if (!published) mutate(); // The same request remains usable after rollback/recovery.
    (void)backend->verifyIntegrity();
    std::cout << "{\"mode\":\"" << mode << "\",\"selected_fault\":" << fault
        << ",\"fault_triggered\":" << (triggered ? "true" : "false")
        << ",\"allocation_attempts\":" << attempts << ",\"published\":" << (published ? "true" : "false")
        << ",\"passed\":true}\n";
}
