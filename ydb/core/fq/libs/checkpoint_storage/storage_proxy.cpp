#include "storage_proxy.h"

#include "gc.h"
#include <ydb/core/fq/libs/config/protos/storage.pb.h>
#include <ydb/core/fq/libs/control_plane_storage/util.h>
#include "ydb_checkpoint_storage.h"
#include "ydb_state_storage.h"

#include <ydb/core/fq/libs/checkpointing_common/defs.h>
#include <ydb/core/fq/libs/checkpoint_storage/events/events.h>

#include <ydb/core/fq/libs/actors/logging/log.h>
#include <ydb/core/fq/libs/ydb/ydb.h>
#include <ydb/core/fq/libs/ydb/util.h>

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

#include <util/stream/file.h>
#include <util/string/join.h>
#include <util/string/strip.h>

namespace NFq {

using namespace NActors;

namespace {

////////////////////////////////////////////////////////////////////////////////

struct TStorageProxyMetrics : public TThrRefBase {
    explicit TStorageProxyMetrics(const ::NMonitoring::TDynamicCounterPtr& counters)
        : Counters(counters)
        , Errors(Counters->GetCounter("Errors", true))
        , Inflight(Counters->GetCounter("Inflight"))
        , LatencyMs(Counters->GetHistogram("LatencyMs", ::NMonitoring::ExplicitHistogram({1, 5, 20, 100, 500, 2000, 10000, 50000})))
    {}

    ::NMonitoring::TDynamicCounterPtr Counters;
    ::NMonitoring::TDynamicCounters::TCounterPtr Errors;
    ::NMonitoring::TDynamicCounters::TCounterPtr Inflight;
    ::NMonitoring::THistogramPtr LatencyMs;
};

using TStorageProxyMetricsPtr = TIntrusivePtr<TStorageProxyMetrics>;

struct TRequestContext : public TThrRefBase {
    TInstant StartTime = TInstant::Now();
    const TStorageProxyMetricsPtr Metrics;
    
    TRequestContext(const TStorageProxyMetricsPtr& metrics)
        : Metrics(metrics) {
        Metrics->Inflight->Inc();
    }

    ~TRequestContext() {
        Metrics->Inflight->Dec();
        Metrics->LatencyMs->Collect((TInstant::Now() - StartTime).MilliSeconds());
    }

    void IncError() {
        Metrics->Errors->Inc();
    }
};

class TStorageProxy : public TActorBootstrapped<TStorageProxy> {
    NConfig::TCheckpointCoordinatorConfig Config;
    NConfig::TCommonConfig CommonConfig;
    NConfig::TYdbStorageConfig StorageConfig;
    TCheckpointStoragePtr CheckpointStorage;
    TStateStoragePtr StateStorage;
    TActorId ActorGC;
    NKikimr::TYdbCredentialsProviderFactory CredentialsProviderFactory;
    TYqSharedResources::TPtr YqSharedResources;
    const TStorageProxyMetricsPtr Metrics;

public:
    explicit TStorageProxy(
        const NConfig::TCheckpointCoordinatorConfig& config,
        const NConfig::TCommonConfig& commonConfig,
        const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
        const TYqSharedResources::TPtr& yqSharedResources,
        const ::NMonitoring::TDynamicCounterPtr& counters);

    void Bootstrap();

    static constexpr char ActorName[] = "YQ_STORAGE_PROXY";

private:
    STRICT_STFUNC(StateFunc,
        hFunc(TEvCheckpointStorage::TEvRegisterCoordinatorRequest, Handle);
        hFunc(TEvCheckpointStorage::TEvCreateCheckpointRequest, Handle);
        hFunc(TEvCheckpointStorage::TEvSetCheckpointPendingCommitStatusRequest, Handle);
        hFunc(TEvCheckpointStorage::TEvCompleteCheckpointRequest, Handle);
        hFunc(TEvCheckpointStorage::TEvAbortCheckpointRequest, Handle);
        hFunc(TEvCheckpointStorage::TEvGetCheckpointsMetadataRequest, Handle);

        hFunc(NYql::NDq::TEvDqCompute::TEvSaveTaskState, Handle);
        hFunc(NYql::NDq::TEvDqCompute::TEvGetTaskState, Handle);
    )

    void Handle(TEvCheckpointStorage::TEvRegisterCoordinatorRequest::TPtr& ev);

    void Handle(TEvCheckpointStorage::TEvCreateCheckpointRequest::TPtr& ev);
    void Handle(TEvCheckpointStorage::TEvSetCheckpointPendingCommitStatusRequest::TPtr& ev);
    void Handle(TEvCheckpointStorage::TEvCompleteCheckpointRequest::TPtr& ev);
    void Handle(TEvCheckpointStorage::TEvAbortCheckpointRequest::TPtr& ev);

    void Handle(TEvCheckpointStorage::TEvGetCheckpointsMetadataRequest::TPtr& ev);

    void Handle(NYql::NDq::TEvDqCompute::TEvSaveTaskState::TPtr& ev);
    void Handle(NYql::NDq::TEvDqCompute::TEvGetTaskState::TPtr& ev);
};

static void FillDefaultParameters(NConfig::TCheckpointCoordinatorConfig& checkpointCoordinatorConfig, NConfig::TYdbStorageConfig& ydbStorageConfig) {
    auto& limits = *checkpointCoordinatorConfig.MutableStateStorageLimits();
    if (!limits.GetMaxGraphCheckpointsSizeBytes()) {
        limits.SetMaxGraphCheckpointsSizeBytes(1099511627776);
    }

    if (!limits.GetMaxTaskStateSizeBytes()) {
        limits.SetMaxTaskStateSizeBytes(1099511627776);
    }

    if (!limits.GetMaxRowSizeBytes()) {
        limits.SetMaxRowSizeBytes(MaxYdbStringValueLength);
    }

    if (!checkpointCoordinatorConfig.GetStorage().GetToken() && checkpointCoordinatorConfig.GetStorage().GetOAuthFile()) {
        checkpointCoordinatorConfig.MutableStorage()->SetToken(StripString(TFileInput(checkpointCoordinatorConfig.GetStorage().GetOAuthFile()).ReadAll()));
    }

    if (!ydbStorageConfig.GetToken() && ydbStorageConfig.GetOAuthFile()) {
        ydbStorageConfig.SetToken(StripString(TFileInput(ydbStorageConfig.GetOAuthFile()).ReadAll()));
    }
}

TStorageProxy::TStorageProxy(
    const NConfig::TCheckpointCoordinatorConfig& config,
    const NConfig::TCommonConfig& commonConfig,
    const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
    const TYqSharedResources::TPtr& yqSharedResources,
    const ::NMonitoring::TDynamicCounterPtr& counters)
    : Config(config)
    , CommonConfig(commonConfig)
    , StorageConfig(Config.GetStorage())
    , CredentialsProviderFactory(credentialsProviderFactory)
    , YqSharedResources(yqSharedResources)
    , Metrics(MakeIntrusive<TStorageProxyMetrics>(counters)) {
    FillDefaultParameters(Config, StorageConfig);
}

void TStorageProxy::Bootstrap() {
    auto ydbConnectionPtr = NewYdbConnection(Config.GetStorage(), CredentialsProviderFactory, YqSharedResources->UserSpaceYdbDriver);
    CheckpointStorage = NewYdbCheckpointStorage(StorageConfig, CreateEntityIdGenerator(CommonConfig.GetIdsPrefix()), ydbConnectionPtr);
    auto issues = CheckpointStorage->Init().GetValueSync();
    if (!issues.Empty()) {
        LOG_STREAMS_STORAGE_SERVICE_ERROR("Failed to init checkpoint storage: " << issues.ToOneLineString());
    }

    StateStorage = NewYdbStateStorage(Config, ydbConnectionPtr);
    issues = StateStorage->Init().GetValueSync();
    if (!issues.Empty()) {
        LOG_STREAMS_STORAGE_SERVICE_ERROR("Failed to init checkpoint state storage: " << issues.ToOneLineString());
    }

    if (Config.GetCheckpointGarbageConfig().GetEnabled()) {
        const auto& gcConfig = Config.GetCheckpointGarbageConfig();
        ActorGC = Register(NewGC(gcConfig, CheckpointStorage, StateStorage).release());
    }

    Become(&TStorageProxy::StateFunc);

    LOG_STREAMS_STORAGE_SERVICE_INFO("Successfully bootstrapped TStorageProxy " << SelfId() << " with connection to "
        << StorageConfig.GetEndpoint().data()
        << ":" << StorageConfig.GetDatabase().data())
}

void TStorageProxy::Handle(TEvCheckpointStorage::TEvRegisterCoordinatorRequest::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);

    const auto* event = ev->Get();
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->CoordinatorId << "] Got TEvRegisterCoordinatorRequest")

    CheckpointStorage->RegisterGraphCoordinator(event->CoordinatorId)
        .Apply([coordinatorId = event->CoordinatorId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context] (const NThreading::TFuture<NYql::TIssues>& issuesFuture) {
            auto response = std::make_unique<TEvCheckpointStorage::TEvRegisterCoordinatorResponse>();
            response->Issues = issuesFuture.GetValue();
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << coordinatorId << "] Failed to register graph: " << response->Issues.ToString())
            } else {
                LOG_STREAMS_STORAGE_SERVICE_AS_INFO(*actorSystem, "[" << coordinatorId << "] Graph registered")
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] Send TEvRegisterCoordinatorResponse")
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(TEvCheckpointStorage::TEvCreateCheckpointRequest::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    const auto* event = ev->Get();
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->CoordinatorId << "] [" << event->CheckpointId << "] Got TEvCreateCheckpointRequest")

    CheckpointStorage->GetTotalCheckpointsStateSize(event->CoordinatorId.GraphId)
        .Apply([checkpointId = event->CheckpointId,
                coordinatorId = event->CoordinatorId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                totalGraphCheckpointsSizeLimit = Config.GetStateStorageLimits().GetMaxGraphCheckpointsSizeBytes(),
                graphDesc = std::move(event->GraphDescription),
                storage = CheckpointStorage,
                context]
               (const NThreading::TFuture<ICheckpointStorage::TGetTotalCheckpointsStateSizeResult>& resultFuture) {
            auto [totalGraphCheckpointsSize, issues] = resultFuture.GetValue();

            if (!issues && totalGraphCheckpointsSize > totalGraphCheckpointsSizeLimit) {
                TStringStream ss;
                ss << "Graph checkpoints size limit exceeded: limit " << totalGraphCheckpointsSizeLimit << ", current checkpoints size: " << totalGraphCheckpointsSize;
                issues.AddIssue(std::move(ss.Str()));
            }
            if (issues) {
                context->IncError();
                return NThreading::MakeFuture(ICheckpointStorage::TCreateCheckpointResult {TString(), std::move(issues) } );
            }
            if (std::holds_alternative<TString>(graphDesc)) {
                return storage->CreateCheckpoint(coordinatorId, checkpointId, std::get<TString>(graphDesc), ECheckpointStatus::Pending);
            } else {
                return storage->CreateCheckpoint(coordinatorId, checkpointId, std::get<NProto::TCheckpointGraphDescription>(graphDesc), ECheckpointStatus::Pending);
            }
        })
        .Apply([checkpointId = event->CheckpointId,
                coordinatorId = event->CoordinatorId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context]
               (const NThreading::TFuture<ICheckpointStorage::TCreateCheckpointResult>& resultFuture) {
            auto [graphDescId, issues] = resultFuture.GetValue();
            auto response = std::make_unique<TEvCheckpointStorage::TEvCreateCheckpointResponse>(checkpointId, std::move(issues), std::move(graphDescId));
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Failed to create checkpoint: " << response->Issues.ToString());
            } else {
                LOG_STREAMS_STORAGE_SERVICE_AS_INFO(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Checkpoint created");
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Send TEvCreateCheckpointResponse");
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(TEvCheckpointStorage::TEvSetCheckpointPendingCommitStatusRequest::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    const auto* event = ev->Get();
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->CoordinatorId << "] [" << event->CheckpointId << "] Got TEvSetCheckpointPendingCommitStatusRequest")
    CheckpointStorage->UpdateCheckpointStatus(event->CoordinatorId, event->CheckpointId, ECheckpointStatus::PendingCommit, ECheckpointStatus::Pending, event->StateSizeBytes)
        .Apply([checkpointId = event->CheckpointId,
                coordinatorId = event->CoordinatorId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context]
               (const NThreading::TFuture<NYql::TIssues>& issuesFuture) {
            auto issues = issuesFuture.GetValue();
            auto response = std::make_unique<TEvCheckpointStorage::TEvSetCheckpointPendingCommitStatusResponse>(checkpointId, std::move(issues));
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Failed to set 'PendingCommit' status: " << response->Issues.ToString())
            } else {
                LOG_STREAMS_STORAGE_SERVICE_AS_INFO(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Status updated to 'PendingCommit'")
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Send TEvSetCheckpointPendingCommitStatusResponse")
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(TEvCheckpointStorage::TEvCompleteCheckpointRequest::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    const auto* event = ev->Get();
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->CoordinatorId << "] [" << event->CheckpointId << "] Got TEvCompleteCheckpointRequest")
    CheckpointStorage->UpdateCheckpointStatus(event->CoordinatorId, event->CheckpointId, ECheckpointStatus::Completed, ECheckpointStatus::PendingCommit, event->StateSizeBytes)
        .Apply([checkpointId = event->CheckpointId,
                coordinatorId = event->CoordinatorId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                type = event->Type,
                gcEnabled = Config.GetCheckpointGarbageConfig().GetEnabled(),
                actorGC = ActorGC,
                actorSystem = TActivationContext::ActorSystem(),
                context]
               (const NThreading::TFuture<NYql::TIssues>& issuesFuture) {
            auto issues = issuesFuture.GetValue();
            auto response = std::make_unique<TEvCheckpointStorage::TEvCompleteCheckpointResponse>(checkpointId, std::move(issues));
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Failed to set 'Completed' status: " << response->Issues.ToString())
            } else {
                LOG_STREAMS_STORAGE_SERVICE_AS_INFO(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Status updated to 'Completed'")
                if (gcEnabled) {
                    auto request = std::make_unique<TEvCheckpointStorage::TEvNewCheckpointSucceeded>(coordinatorId, checkpointId, type);
                    LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Send TEvNewCheckpointSucceeded")
                    actorSystem->Send(actorGC, request.release(), 0);
                }
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Send TEvCompleteCheckpointResponse")
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(TEvCheckpointStorage::TEvAbortCheckpointRequest::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    const auto* event = ev->Get();
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->CoordinatorId << "] [" << event->CheckpointId << "] Got TEvAbortCheckpointRequest")
    CheckpointStorage->AbortCheckpoint(event->CoordinatorId,event->CheckpointId)
        .Apply([checkpointId = event->CheckpointId,
                coordinatorId = event->CoordinatorId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context] (const NThreading::TFuture<NYql::TIssues>& issuesFuture) {
            auto issues = issuesFuture.GetValue();
            auto response = std::make_unique<TEvCheckpointStorage::TEvAbortCheckpointResponse>(checkpointId, std::move(issues));
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Failed to abort checkpoint: " << response->Issues.ToString())
            } else {
                LOG_STREAMS_STORAGE_SERVICE_AS_INFO(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Checkpoint aborted")
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << coordinatorId << "] [" << checkpointId << "] Send TEvAbortCheckpointResponse")
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(TEvCheckpointStorage::TEvGetCheckpointsMetadataRequest::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    const auto* event = ev->Get();
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->GraphId << "] Got TEvGetCheckpointsMetadataRequest");
    CheckpointStorage->GetCheckpoints(event->GraphId, event->Statuses, event->Limit, event->LoadGraphDescription)
        .Apply([graphId = event->GraphId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context] (const NThreading::TFuture<ICheckpointStorage::TGetCheckpointsResult>& futureResult) {
            auto result = futureResult.GetValue();
            auto response = std::make_unique<TEvCheckpointStorage::TEvGetCheckpointsMetadataResponse>(result.first, result.second);
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << graphId << "] Failed to get checkpoints: " << response->Issues.ToString())
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << graphId << "] Send TEvGetCheckpointsMetadataResponse")
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(NYql::NDq::TEvDqCompute::TEvSaveTaskState::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    auto* event = ev->Get();
    const auto checkpointId = TCheckpointId(event->Checkpoint.GetGeneration(), event->Checkpoint.GetId());
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->GraphId << "] [" << checkpointId << "] Got TEvSaveTaskState: task " << event->TaskId);

    const size_t stateSize = event->State.ByteSizeLong();
    if (stateSize > Config.GetStateStorageLimits().GetMaxTaskStateSizeBytes()) {
        LOG_STREAMS_STORAGE_SERVICE_WARN("[" << event->GraphId << "] [" << checkpointId << "] Won't save task state because it's too big: task: " << event->TaskId
            << ", state size: " << stateSize << "/" << Config.GetStateStorageLimits().GetMaxTaskStateSizeBytes());
        auto response = std::make_unique<NYql::NDq::TEvDqCompute::TEvSaveTaskStateResult>();
        response->Record.MutableCheckpoint()->SetGeneration(checkpointId.CoordinatorGeneration);
        response->Record.MutableCheckpoint()->SetId(checkpointId.SeqNo);
        response->Record.SetStateSizeBytes(0);
        response->Record.SetTaskId(event->TaskId);
        response->Record.SetStatus(NYql::NDqProto::TEvSaveTaskStateResult::STATE_TOO_BIG);
        Send(ev->Sender, response.release());
        return;
    }

    StateStorage->SaveState(event->TaskId, event->GraphId, checkpointId, event->State)
        .Apply([graphId = event->GraphId,
                checkpointId,
                taskId = event->TaskId,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context](const NThreading::TFuture<IStateStorage::TSaveStateResult>& futureResult) {
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << graphId << "] [" << checkpointId << "] TEvSaveTaskState Apply: task: " << taskId)
            const auto& issues = futureResult.GetValue().second;
            auto response = std::make_unique<NYql::NDq::TEvDqCompute::TEvSaveTaskStateResult>();
            response->Record.MutableCheckpoint()->SetGeneration(checkpointId.CoordinatorGeneration);
            response->Record.MutableCheckpoint()->SetId(checkpointId.SeqNo);
            response->Record.SetStateSizeBytes(futureResult.GetValue().first);
            response->Record.SetTaskId(taskId);

            if (issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << graphId << "] [" << checkpointId << "] Failed to save task state: task: " << taskId << ", issues: " << issues.ToString())
                response->Record.SetStatus(NYql::NDqProto::TEvSaveTaskStateResult::STORAGE_ERROR);
            } else {
                response->Record.SetStatus(NYql::NDqProto::TEvSaveTaskStateResult::OK);
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << graphId << "] [" << checkpointId << "] Send TEvSaveTaskStateResult: task: " << taskId)
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

void TStorageProxy::Handle(NYql::NDq::TEvDqCompute::TEvGetTaskState::TPtr& ev) {
    auto context = MakeIntrusive<TRequestContext>(Metrics);
    const auto* event = ev->Get();
    const auto checkpointId = TCheckpointId(event->Checkpoint.GetGeneration(), event->Checkpoint.GetId());
    LOG_STREAMS_STORAGE_SERVICE_DEBUG("[" << event->GraphId << "] [" << checkpointId << "] Got TEvGetTaskState: tasks {" << JoinSeq(", ", event->TaskIds) << "}");

    StateStorage->GetState(event->TaskIds, event->GraphId, checkpointId)
        .Apply([checkpointId = event->Checkpoint,
                generation = event->Generation,
                graphId = event->GraphId,
                taskIds = event->TaskIds,
                cookie = ev->Cookie,
                sender = ev->Sender,
                actorSystem = TActivationContext::ActorSystem(),
                context](const NThreading::TFuture<IStateStorage::TGetStateResult>& resultFuture) {
            auto result = resultFuture.GetValue();

            auto response = std::make_unique<NYql::NDq::TEvDqCompute::TEvGetTaskStateResult>(checkpointId, result.second, generation);
            std::swap(response->States, result.first);
            if (response->Issues) {
                context->IncError();
                LOG_STREAMS_STORAGE_SERVICE_AS_WARN(*actorSystem, "[" << graphId << "] [" << checkpointId << "] Failed to get task state: tasks: {" << JoinSeq(", ", taskIds) << "}, issues: " << response->Issues.ToString());
            }
            LOG_STREAMS_STORAGE_SERVICE_AS_DEBUG(*actorSystem, "[" << graphId << "] [" << checkpointId << "] Send TEvGetTaskStateResult: tasks: {" << JoinSeq(", ", taskIds) << "}");
            actorSystem->Send(sender, response.release(), 0, cookie);
        });
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<NActors::IActor> NewStorageProxy(
    const NConfig::TCheckpointCoordinatorConfig& config,
    const NConfig::TCommonConfig& commonConfig,
    const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
    const TYqSharedResources::TPtr& yqSharedResources,
    const ::NMonitoring::TDynamicCounterPtr& counters)
{
    return std::unique_ptr<NActors::IActor>(new TStorageProxy(config, commonConfig, credentialsProviderFactory, yqSharedResources, counters));
}

} // namespace NFq
