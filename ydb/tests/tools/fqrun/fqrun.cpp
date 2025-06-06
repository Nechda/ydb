#include <library/cpp/colorizer/colors.h>
#include <library/cpp/getopt/last_getopt.h>

#include <util/datetime/base.h>

#include <ydb/core/blob_depot/mon_main.h>
#include <ydb/library/yql/providers/pq/gateway/dummy/yql_pq_dummy_gateway.h>
#include <ydb/tests/tools/fqrun/src/fq_runner.h>
#include <ydb/tests/tools/kqprun/runlib/application.h>
#include <ydb/tests/tools/kqprun/runlib/utils.h>

#ifdef PROFILE_MEMORY_ALLOCATIONS
#include <library/cpp/lfalloc/alloc_profiler/profiler.h>
#endif

using namespace NKikimrRun;

namespace NFqRun {

namespace {

struct TExecutionOptions {
    enum class EExecutionCase {
        Stream,
        Analytics,
        AsyncStream,
        AsyncAnalytics
    };

    std::vector<TString> Queries;
    std::vector<FederatedQuery::ConnectionContent> Connections;
    std::vector<FederatedQuery::BindingContent> Bindings;

    ui32 LoopCount = 1;
    TDuration QueryDelay;
    TDuration LoopDelay;
    bool ContinueAfterFail = false;

    std::vector<EExecutionCase> ExecutionCases;
    std::vector<FederatedQuery::ExecuteMode> QueryActions;
    std::vector<TString> Scopes;

    bool HasResults() const {
        for (size_t i = 0; i < Queries.size(); ++i) {
            if (GetQueryAction(i) != FederatedQuery::ExecuteMode::RUN) {
                continue;
            }
            const auto executionCase = GetExecutionCase(i);
            if (executionCase == EExecutionCase::Stream || executionCase == EExecutionCase::Analytics) {
                return true;
            }
        }
        return false;
    }

    bool HasExecutionCase(EExecutionCase executionCase) const {
        for (size_t i = 0; i < Queries.size(); ++i) {
            if (GetExecutionCase(i) == executionCase) {
                return true;
            }
        }
        return false;
    }

    EExecutionCase GetExecutionCase(size_t index) const {
        return GetValue(index, ExecutionCases, EExecutionCase::Stream);
    }

    FederatedQuery::ExecuteMode GetQueryAction(size_t index) const {
        return GetValue(index, QueryActions, FederatedQuery::ExecuteMode::RUN);
    }

    TString GetScope(size_t index) const {
        return GetValue<TString>(index, Scopes, "fqrun");
    }

    TRequestOptions GetQueryOptions(size_t index, ui64 queryId) const {
        Y_ABORT_UNLESS(index < Queries.size());

        const auto executionCase = GetExecutionCase(index);
        const bool isAnalytics = executionCase == EExecutionCase::Analytics || executionCase == EExecutionCase::AsyncAnalytics;
        return {
            .Query = Queries[index],
            .Action = GetQueryAction(index),
            .Type = isAnalytics ? FederatedQuery::QueryContent::ANALYTICS : FederatedQuery::QueryContent::STREAMING,
            .QueryId = queryId,
            .FqOptions = {
                .Scope = GetScope(index)
            }
        };
    }

    void Validate(const TRunnerOptions& runnerOptions) const {
        if (Queries.empty() && Connections.empty() && Bindings.empty() && !runnerOptions.FqSettings.MonitoringEnabled && !runnerOptions.FqSettings.GrpcEnabled) {
            ythrow yexception() << "Nothing to execute and is not running as daemon";
        }
        ValidateOptionsSizes(runnerOptions);
        ValidateAsyncOptions(runnerOptions.FqSettings.AsyncQueriesSettings);
        ValidateTraceOpt(runnerOptions);
    }

private:
    void ValidateOptionsSizes(const TRunnerOptions& runnerOptions) const {
        const auto checker = [numberQueries = Queries.size()](size_t checkSize, const TString& optionName) {
            if (checkSize > numberQueries) {
                ythrow yexception() << "Too many " << optionName << ". Specified " << checkSize << ", when number of queries is " << numberQueries;
            }
        };

        checker(ExecutionCases.size(), "execution cases");
        checker(QueryActions.size(), "query actions");
        checker(Scopes.size(), "query scopes");
        checker(runnerOptions.AstOutputs.size(), "ast output files");
        checker(runnerOptions.PlanOutputs.size(), "plan output files");
    }

    ui64 GetNumberOfQueries() const {
        if (Queries.empty()) {
            return 0;
        }
        return LoopCount ? LoopCount * Queries.size() : std::numeric_limits<ui64>::max();
    }

    void ValidateAsyncOptions(const TAsyncQueriesSettings& asyncQueriesSettings) const {
        if (asyncQueriesSettings.InFlightLimit && !HasExecutionCase(EExecutionCase::AsyncStream) && !HasExecutionCase(EExecutionCase::AsyncAnalytics)) {
            ythrow yexception() << "In flight limit can not be used without async queries";
        }

        NColorizer::TColors colors = NColorizer::AutoColors(Cout);
        const auto numberOfQueries = GetNumberOfQueries();
        if (asyncQueriesSettings.InFlightLimit && asyncQueriesSettings.InFlightLimit > numberOfQueries) {
            Cout << colors.Red() << "Warning: inflight limit is " << asyncQueriesSettings.InFlightLimit << ", that is larger than max possible number of queries " << numberOfQueries << colors.Default() << Endl;
        }
    }

    void ValidateTraceOpt(const TRunnerOptions& runnerOptions) const {
        if (runnerOptions.TraceOptAll && !runnerOptions.TraceOptIds.empty()) {
            ythrow yexception() << "Trace opt ids can not be used with trace opt all flag";
        }

        const auto numberOfQueries = GetNumberOfQueries();
        for (auto id : runnerOptions.TraceOptIds) {
            if (id >= numberOfQueries) {
                ythrow yexception() << "Trace opt id " << id << " should be less than number of queries " << numberOfQueries;
            }
        }
    }
};

void RunArgumentQuery(size_t index, ui64 queryId, const TExecutionOptions& executionOptions, TFqRunner& runner) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    switch (executionOptions.GetExecutionCase(index)) {
        case TExecutionOptions::EExecutionCase::Analytics:
        case TExecutionOptions::EExecutionCase::Stream: {
            if (!runner.ExecuteQuery(executionOptions.GetQueryOptions(index, queryId))) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Query execution failed";
            }
            Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Fetching query results..." << colors.Default() << Endl;
            if (!runner.FetchQueryResults()) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Fetch query results failed";
            }
            break;
        }

        case TExecutionOptions::EExecutionCase::AsyncAnalytics:
        case TExecutionOptions::EExecutionCase::AsyncStream: {
            runner.ExecuteQueryAsync(executionOptions.GetQueryOptions(index, queryId));
            break;
        }
    }
}

void RunArgumentQueries(const TExecutionOptions& executionOptions, TFqRunner& runner) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    std::unordered_set<TString> scopes;
    scopes.reserve(executionOptions.Scopes.size());
    for (size_t i = 0; i < executionOptions.Queries.size(); ++i){
        scopes.emplace(executionOptions.GetScope(i));
    }

    if (!executionOptions.Connections.empty()) {
        Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Creating connections..." << colors.Default() << Endl;
        for (const auto& scope : scopes) {
            if (!runner.CreateConnections(executionOptions.Connections, {.Scope = scope})) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Failed to create connections for scope " << scope;
            }
        }
    }

    if (!executionOptions.Bindings.empty()) {
        Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Creating bindings..." << colors.Default() << Endl;
        for (const auto& scope : scopes) {
            if (!runner.CreateBindings(executionOptions.Bindings, {.Scope = scope})) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Failed to create bindings for scope " << scope;
            }
        }
    }

    const size_t numberQueries = executionOptions.Queries.size();
    if (!numberQueries) {
        return;
    }

    const size_t numberLoops = executionOptions.LoopCount;
    for (size_t queryId = 0; queryId < numberQueries * numberLoops || numberLoops == 0; ++queryId) {
        size_t idx = queryId % numberQueries;
        if (queryId > 0) {
            Sleep(idx == 0 ? executionOptions.LoopDelay : executionOptions.QueryDelay);
        }

        const size_t loopId = queryId / numberQueries;
        const auto executionCase = executionOptions.GetExecutionCase(idx);
        if (executionCase != TExecutionOptions::EExecutionCase::AsyncAnalytics && executionCase != TExecutionOptions::EExecutionCase::AsyncStream) {
            Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Executing query";
            if (numberQueries > 1) {
                Cout << " " << idx;
            }
            if (numberLoops != 1) {
                Cout << ", loop " << loopId;
            }
            Cout << "..." << colors.Default() << Endl;
        }

        try {
            RunArgumentQuery(idx, queryId, executionOptions, runner);
        } catch (const yexception& exception) {
            if (executionOptions.ContinueAfterFail) {
                Cerr << colors.Red() <<  CurrentExceptionMessage() << colors.Default() << Endl;
            } else {
                throw exception;
            }
        }
    }
    runner.FinalizeRunner();

    if (executionOptions.HasResults()) {
        try {
            runner.PrintQueryResults();
        } catch (...) {
            ythrow yexception() << "Failed to print script results, reason:\n" <<  CurrentExceptionMessage();
        }
    }
}

void RunAsDaemon() {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Initialization finished" << colors.Default() << Endl;
    while (true) {
        Sleep(TDuration::Seconds(1));
    }
}

void RunScript(const TExecutionOptions& executionOptions, const TRunnerOptions& runnerOptions) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Initialization of fq runner..." << colors.Default() << Endl;
    TFqRunner runner(runnerOptions);

    try {
        RunArgumentQueries(executionOptions, runner);
    } catch (const yexception& exception) {
        if (runnerOptions.FqSettings.MonitoringEnabled) {
            Cerr << colors.Red() <<  CurrentExceptionMessage() << colors.Default() << Endl;
        } else {
            throw exception;
        }
    }

    if (runnerOptions.FqSettings.MonitoringEnabled || runnerOptions.FqSettings.GrpcEnabled) {
        RunAsDaemon();
    }

    Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Finalization of fq runner..." << colors.Default() << Endl;
}

class TMain : public TMainBase {
    using TBase = TMainBase;
    using EVerbose = TFqSetupSettings::EVerbose;

protected:
    void RegisterLogOptions(NLastGetopt::TOpts& options) override {
        TBase::RegisterLogOptions(options);

        options.AddLongOption("log-fq", "FQ components log priority")
            .RequiredArgument("priority")
            .StoreMappedResultT<TString>(&FqLogPriority, GetLogPrioritiesMap("log-fq"));
    }

    void RegisterOptions(NLastGetopt::TOpts& options) override {
        options.SetTitle("FqRun -- tool to execute stream queries through FQ proxy");
        options.AddHelpOption('h');
        options.SetFreeArgsNum(0);

        // Inputs

        options.AddLongOption('p', "query", "Query to execute")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                ExecutionOptions.Queries.emplace_back(LoadFile(option->CurVal()));
            });

        options.AddLongOption('s', "sql", "Query SQL text to execute")
            .RequiredArgument("str")
            .AppendTo(&ExecutionOptions.Queries);

        options.AddLongOption('c', "connection", "External datasource connection protobuf FederatedQuery::ConnectionContent")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                auto& connection = ExecutionOptions.Connections.emplace_back();
                const TString file(TString(option->CurValOrDef()));
                if (!google::protobuf::TextFormat::ParseFromString(LoadFile(file), &connection)) {
                    ythrow yexception() << "Bad format of FQ connection in file '" << file << "'";
                }
                SetupAcl(connection.mutable_acl());
            });

        options.AddLongOption('b', "binding", "External datasource binding protobuf FederatedQuery::BindingContent")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                auto& binding = ExecutionOptions.Bindings.emplace_back();
                const TString file(TString(option->CurValOrDef()));
                if (!google::protobuf::TextFormat::ParseFromString(LoadFile(file), &binding)) {
                    ythrow yexception() << "Bad format of FQ binding in file '" << file << "'";
                }
                SetupAcl(binding.mutable_acl());
            });

        options.AddLongOption("cfg", "File with actor system config (TActorSystemConfig), use '-' for default")
            .RequiredArgument("file")
            .DefaultValue("./configuration/app_config.conf")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                if (!google::protobuf::TextFormat::ParseFromString(LoadFile(TString(option->CurValOrDef())), &RunnerOptions.FqSettings.AppConfig)) {
                    ythrow yexception() << "Bad format of app configuration";
                }
            });

        options.AddLongOption("emulate-s3", "Enable readings by s3 provider from files, `bucket` value in connection - path to folder with files")
            .NoArgument()
            .SetFlag(&RunnerOptions.FqSettings.EmulateS3);

        options.AddLongOption("emulate-pq", "Emulate YDS with local file, accepts list of tables to emulate with following format: topic@file (can be used in query from cluster `pq`)")
            .RequiredArgument("topic@file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                TStringBuf topicName, others;
                TStringBuf(option->CurVal()).Split('@', topicName, others);

                TStringBuf path, partitionCountStr;
                TStringBuf(others).Split(':', path, partitionCountStr);
                size_t partitionCount = !partitionCountStr.empty() ? FromString<size_t>(partitionCountStr) : 1;
                if (!partitionCount) {
                    ythrow yexception() << "Topic partition count should be at least one";
                }
                if (topicName.empty() || path.empty()) {
                    ythrow yexception() << "Incorrect PQ file mapping, expected form topic@path[:partitions_count]";
                }
                if (!PqFilesMapping.emplace(topicName, NYql::TDummyTopic("pq", TString(topicName), TString(path), partitionCount)).second) {
                    ythrow yexception() << "Got duplicated topic name: " << topicName;
                }
            });

        options.AddLongOption("cancel-on-file-finish", "Cancel emulate YDS topics when topic file finished")
            .RequiredArgument("topic")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                TopicsSettings[option->CurVal()].CancelOnFileFinish = true;
            });

        // Outputs

        options.AddLongOption('T', "trace-opt", "Print AST in the begin of each transformation")
            .OptionalArgument("query-id")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                const auto value = option->CurVal();
                if (!value) {
                    RunnerOptions.TraceOptAll = true;
                } else if (!RunnerOptions.TraceOptIds.emplace(FromString<ui64>(value)).second) {
                    ythrow yexception() << "Got duplicated trace opt index: " << value;
                }
                RunnerOptions.FqSettings.EnableTraceOpt = true;
            });

        options.AddLongOption("result-file", "File with query results (use '-' to write in stdout)")
            .RequiredArgument("file")
            .DefaultValue("-")
            .StoreMappedResultT<TString>(&RunnerOptions.ResultOutput, &GetDefaultOutput);

        TChoices<EResultOutputFormat> resultFormat({
            {"rows", EResultOutputFormat::RowsJson},
            {"full-json", EResultOutputFormat::FullJson},
            {"full-proto", EResultOutputFormat::FullProto}
        });
        options.AddLongOption('R', "result-format", "Query result format")
            .RequiredArgument("result-format")
            .DefaultValue("rows")
            .Choices(resultFormat.GetChoices())
            .StoreMappedResultT<TString>(&RunnerOptions.ResultOutputFormat, resultFormat);

        options.AddLongOption("ast-file", "File with query ast (use '-' to write in stdout)")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.AstOutputs.emplace_back(GetDefaultOutput(TString(option->CurValOrDef())));
            });

        options.AddLongOption("plan-file", "File with query plan (use '-' to write in stdout)")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.PlanOutputs.emplace_back(GetDefaultOutput(TString(option->CurValOrDef())));
            });

        options.AddLongOption("canonical-output", "Make ast and plan output suitable for canonization (replace volatile data such as endpoints with stable one)")
            .NoArgument()
            .SetFlag(&RunnerOptions.CanonicalOutput);

        // Pipeline settings

        TChoices<TExecutionOptions::EExecutionCase> executionCase({
            {"stream", TExecutionOptions::EExecutionCase::Stream},
            {"analytics", TExecutionOptions::EExecutionCase::Analytics},
            {"async-stream", TExecutionOptions::EExecutionCase::AsyncStream},
            {"async-analytics", TExecutionOptions::EExecutionCase::AsyncAnalytics}
        });
        options.AddLongOption('C', "execution-case", "Type of query for -p argument")
            .RequiredArgument("query-type")
            .Choices(executionCase.GetChoices())
            .Handler1([this, executionCase](const NLastGetopt::TOptsParser* option) {
                TString choice(option->CurValOrDef());
                ExecutionOptions.ExecutionCases.emplace_back(executionCase(choice));
            });

        options.AddLongOption("inflight-limit", "In flight limit for async queries (use 0 for unlimited)")
            .RequiredArgument("uint")
            .DefaultValue(0)
            .StoreResult(&RunnerOptions.FqSettings.AsyncQueriesSettings.InFlightLimit);

        options.AddLongOption("verbose", TStringBuilder() << "Common verbose level (max level " << static_cast<ui32>(EVerbose::Max) - 1 << ")")
            .RequiredArgument("uint")
            .DefaultValue(static_cast<ui8>(EVerbose::Info))
            .StoreMappedResultT<ui8>(&RunnerOptions.FqSettings.VerboseLevel, [](ui8 value) {
                return static_cast<EVerbose>(std::min(value, static_cast<ui8>(EVerbose::Max)));
            });

        TChoices<TAsyncQueriesSettings::EVerbose> verbose({
            {"each-query", TAsyncQueriesSettings::EVerbose::EachQuery},
            {"final", TAsyncQueriesSettings::EVerbose::Final}
        });
        options.AddLongOption("async-verbose", "Verbose type for async queries")
            .RequiredArgument("type")
            .DefaultValue("each-query")
            .Choices(verbose.GetChoices())
            .StoreMappedResultT<TString>(&RunnerOptions.FqSettings.AsyncQueriesSettings.Verbose, verbose);

        options.AddLongOption("ping-period", "Query ping period in milliseconds")
            .RequiredArgument("uint")
            .DefaultValue(100)
            .StoreMappedResultT<ui64>(&RunnerOptions.PingPeriod, &TDuration::MilliSeconds<ui64>);

        TChoices<FederatedQuery::ExecuteMode> queryAction({
            {"save", FederatedQuery::ExecuteMode::SAVE},
            {"parse", FederatedQuery::ExecuteMode::PARSE},
            {"compile", FederatedQuery::ExecuteMode::COMPILE},
            {"validate", FederatedQuery::ExecuteMode::VALIDATE},
            {"explain", FederatedQuery::ExecuteMode::EXPLAIN},
            {"run", FederatedQuery::ExecuteMode::RUN}
        });
        options.AddLongOption('A', "action", "Query execute action")
            .RequiredArgument("action")
            .DefaultValue("run")
            .Choices(queryAction.GetChoices())
            .Handler1([this, queryAction](const NLastGetopt::TOptsParser* option) {
                TString choice(option->CurValOrDef());
                ExecutionOptions.QueryActions.emplace_back(queryAction(choice));
            });

        options.AddLongOption("loop-count", "Number of runs of the query (use 0 to start infinite loop)")
            .RequiredArgument("uint")
            .DefaultValue(ExecutionOptions.LoopCount)
            .StoreResult(&ExecutionOptions.LoopCount);

        options.AddLongOption("loop-delay", "Delay in milliseconds between loop steps")
            .RequiredArgument("uint")
            .DefaultValue(0)
            .StoreMappedResultT<ui64>(&ExecutionOptions.LoopDelay, &TDuration::MilliSeconds<ui64>);

        options.AddLongOption("query-delay", "Delay in milliseconds between queries starts")
            .RequiredArgument("uint")
            .DefaultValue(0)
            .StoreMappedResultT<ui64>(&ExecutionOptions.QueryDelay, &TDuration::MilliSeconds<ui64>);

        options.AddLongOption("continue-after-fail", "Don't not stop requests execution after fails")
            .NoArgument()
            .SetFlag(&ExecutionOptions.ContinueAfterFail);

        options.AddLongOption('S', "scope-id", "Query scope id")
            .RequiredArgument("scope")
            .AppendTo(&ExecutionOptions.Scopes);

        // Cluster settings

        options.AddLongOption("cp-storage-db", "Start real control plane storage instead of in memory (will use local database by default), token variable CP_STORAGE_TOKEN")
            .OptionalArgument("database@endpoint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.FqSettings.EnableCpStorage = true;
                if (const auto value = option->CurVal()) {
                    RunnerOptions.FqSettings.CpStorageDatabase = TExternalDatabase::Parse(value, "CP_STORAGE_TOKEN");
                }
            });

        options.AddLongOption("checkpoints-db", "Start checkpoint coordinator (will use local database by default), token variable CHECKPOINTS_TOKEN")
            .OptionalArgument("database@endpoint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.FqSettings.EnableCheckpoints = true;
                if (const auto value = option->CurVal()) {
                    RunnerOptions.FqSettings.CheckpointsDatabase = TExternalDatabase::Parse(value, "CHECKPOINTS_TOKEN");
                }
            });

        options.AddLongOption("quotas-db", "Start FQ quotas service and rate limiter (will be created local rate limiter by default), token variable QUOTAS_TOKEN")
            .OptionalArgument("database@endpoint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.FqSettings.EnableQuotas = true;
                if (const auto value = option->CurVal()) {
                    RunnerOptions.FqSettings.RateLimiterDatabase = TExternalDatabase::Parse(value, "QUOTAS_TOKEN");
                }
            });

        options.AddLongOption("row-dispatcher-db", "Use real coordinator for row dispatcher (will use local database by default), token variable ROW_DISPATCHER_TOKEN")
            .OptionalArgument("database@endpoint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.FqSettings.EnableRemoteRd = true;
                if (const auto value = option->CurVal()) {
                    RunnerOptions.FqSettings.RowDispatcherDatabase = TExternalDatabase::Parse(value, "ROW_DISPATCHER_TOKEN");
                }
            });

        options.AddLongOption("single-compute-db", "Enable single compute database for analytics queries (will use local database by default), token variable YDB_COMPUTE_TOKEN")
            .OptionalArgument("database@endpoint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.FqSettings.EnableYdbCompute = true;
                if (const auto value = option->CurVal()) {
                    RunnerOptions.FqSettings.SingleComputeDatabase = TExternalDatabase::Parse(value, "YDB_COMPUTE_TOKEN");
                }
            });

        options.AddLongOption("shared-compute-db", "Add shared compute database for analytics queries, token variable YDB_COMPUTE_TOKEN")
            .RequiredArgument("database@endpoint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                RunnerOptions.FqSettings.EnableYdbCompute = true;
                RunnerOptions.FqSettings.SharedComputeDatabases.emplace_back(TExternalDatabase::Parse(option->CurVal(), "YDB_COMPUTE_TOKEN"));
            });
        options.MutuallyExclusive("single-compute-db", "shared-compute-db");

        RegisterKikimrOptions(options, RunnerOptions.FqSettings);
    }

    int DoRun(NLastGetopt::TOptsParseResult&&) override {
        ExecutionOptions.Validate(RunnerOptions);

        if (ExecutionOptions.HasExecutionCase(TExecutionOptions::EExecutionCase::Analytics) || ExecutionOptions.HasExecutionCase(TExecutionOptions::EExecutionCase::AsyncAnalytics)) {
            RunnerOptions.FqSettings.EnableYdbCompute = true;
        }

        RunnerOptions.FqSettings.YqlToken = GetEnv(YQL_TOKEN_VARIABLE);
        RunnerOptions.FqSettings.FunctionRegistry = CreateFunctionRegistry().Get();

        auto& fqConfig = *RunnerOptions.FqSettings.AppConfig.MutableFederatedQueryConfig();
        auto& gatewayConfig = *fqConfig.mutable_gateways();
        FillTokens(gatewayConfig.mutable_pq());
        FillTokens(gatewayConfig.mutable_s3());
        FillTokens(gatewayConfig.mutable_generic());
        FillTokens(gatewayConfig.mutable_ydb());
        FillTokens(gatewayConfig.mutable_solomon());

        fqConfig.MutablePendingFetcher()->SetPendingFetchPeriodMs(RunnerOptions.PingPeriod.MilliSeconds());

        SetupLogsConfig();

        if (!PqFilesMapping.empty()) {
            auto fileGateway = MakeIntrusive<NYql::TDummyPqGateway>();
            for (auto [_, topic] : PqFilesMapping) {
                if (const auto it = TopicsSettings.find(topic.TopicName); it != TopicsSettings.end()) {
                    topic.CancelOnFileFinish = it->second.CancelOnFileFinish;
                    TopicsSettings.erase(it);
                }
                fileGateway->AddDummyTopic(topic);
            }
            RunnerOptions.FqSettings.PqGatewayFactory = CreatePqFileGatewayFactory(fileGateway);
        }
        if (!TopicsSettings.empty()) {
            ythrow yexception() << "Found topic settings for not existing topic: '" << TopicsSettings.begin()->first << "'";
        }

#ifdef PROFILE_MEMORY_ALLOCATIONS
        if (RunnerOptions.FqSettings.VerboseLevel >= EVerbose::Info) {
            Cout << CoutColors.Cyan() << "Starting profile memory allocations" << CoutColors.Default() << Endl;
        }
        NAllocProfiler::StartAllocationSampling(true);
#else
        if (ProfileAllocationsOutput) {
            ythrow yexception() << "Profile memory allocations disabled, please rebuild fqrun with flag `-D PROFILE_MEMORY_ALLOCATIONS`";
        }
#endif

        RunScript(ExecutionOptions, RunnerOptions);

#ifdef PROFILE_MEMORY_ALLOCATIONS
        if (RunnerOptions.FqSettings.VerboseLevel >= EVerbose::Info) {
            Cout << CoutColors.Cyan() << "Finishing profile memory allocations" << CoutColors.Default() << Endl;
        }
        FinishProfileMemoryAllocations();
#endif

        return 0;
    }

private:
    template <typename TGatewayConfig>
    void FillTokens(TGatewayConfig* gateway) const {
        for (auto& cluster : *gateway->mutable_clustermapping()) {
            if (!cluster.GetToken()) {
                cluster.SetToken(RunnerOptions.FqSettings.YqlToken);
            }
        }
    }

    void SetupLogsConfig() {
        auto& logConfig = *RunnerOptions.FqSettings.AppConfig.MutableLogConfig();

        if (DefaultLogPriority) {
            logConfig.SetDefaultLevel(*DefaultLogPriority);
        }

        if (FqLogPriority) {
            std::unordered_map<NKikimrServices::EServiceKikimr, NActors::NLog::EPriority> fqLogPriorities;
            std::unordered_set<TString> prefixes = {
                "FQ_", "YQ_", "STREAMS", "PUBLIC_HTTP"
            };
            auto descriptor = NKikimrServices::EServiceKikimr_descriptor();
            for (int i = 0; i < descriptor->value_count(); ++i) {
                const auto service = static_cast<NKikimrServices::EServiceKikimr>(descriptor->value(i)->number());
                const auto& servicceStr = NKikimrServices::EServiceKikimr_Name(service);
                for (const auto& prefix : prefixes) {
                    if (servicceStr.StartsWith(prefix)) {
                        fqLogPriorities.emplace(service, *FqLogPriority);
                        break;
                    }
                }
            }
            ModifyLogPriorities(fqLogPriorities, logConfig);
        }

        ModifyLogPriorities(LogPriorities, logConfig);
    }

private:
    TExecutionOptions ExecutionOptions;
    TRunnerOptions RunnerOptions;

    struct TTopicSettings {
        bool CancelOnFileFinish = false;
    };
    std::unordered_map<TString, TTopicSettings> TopicsSettings;
    std::unordered_map<TString, NYql::TDummyTopic> PqFilesMapping;

    std::optional<NActors::NLog::EPriority> FqLogPriority;
};

}  // anonymous namespace

}  // namespace NFqRun

int main(int argc, const char* argv[]) {
    SetupSignalActions();

    try {
        NFqRun::TMain().Run(argc, argv);
    } catch (...) {
        NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

        Cerr << colors.Red() <<  CurrentExceptionMessage() << colors.Default() << Endl;
        return 1;
    }
}
