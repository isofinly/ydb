#include "json_pipe_req.h"
#include "log.h"
#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>

namespace NKikimr::NViewer {

NTabletPipe::TClientConfig TViewerPipeClient::GetPipeClientConfig() {
    NTabletPipe::TClientConfig clientConfig;
    if (WithRetry) {
        clientConfig.RetryPolicy = {.RetryLimitCount = 3};
    }
    return clientConfig;
}

TViewerPipeClient::~TViewerPipeClient() = default;

TViewerPipeClient::TViewerPipeClient() = default;

TViewerPipeClient::TViewerPipeClient(NWilson::TTraceId traceId) {
    if (traceId) {
        Span = {TComponentTracingLevels::THttp::TopLevel, std::move(traceId), "viewer", NWilson::EFlags::AUTO_END};
    }
}

void TViewerPipeClient::BuildParamsFromJson(TStringBuf data) {
    NJson::TJsonValue jsonData;
    if (NJson::ReadJsonTree(data, &jsonData)) {
        if (jsonData.IsMap()) {
            for (const auto& [key, value] : jsonData.GetMap()) {
                switch (value.GetType()) {
                    case NJson::EJsonValueType::JSON_STRING:
                    case NJson::EJsonValueType::JSON_INTEGER:
                    case NJson::EJsonValueType::JSON_UINTEGER:
                    case NJson::EJsonValueType::JSON_DOUBLE:
                    case NJson::EJsonValueType::JSON_BOOLEAN:
                        Params.InsertUnescaped(key, value.GetStringRobust());
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

void TViewerPipeClient::SetupTracing(const TString& handlerName) {
    auto request = GetRequest();
    NWilson::TTraceId traceId;
    TString traceparent = request.GetHeader("traceparent");
    if (traceparent) {
        traceId = NWilson::TTraceId::FromTraceparentHeader(traceparent, TComponentTracingLevels::ProductionVerbose);
    }
    TString wantTrace = request.GetHeader("X-Want-Trace");
    TString traceVerbosity = request.GetHeader("X-Trace-Verbosity");
    TString traceTTL = request.GetHeader("X-Trace-TTL");
    if (!traceId && (FromStringWithDefault<bool>(wantTrace) || !traceVerbosity.empty() || !traceTTL.empty())) {
        ui8 verbosity = TComponentTracingLevels::ProductionVerbose;
        if (traceVerbosity) {
            verbosity = FromStringWithDefault<ui8>(traceVerbosity, verbosity);
            verbosity = std::min(verbosity, NWilson::TTraceId::MAX_VERBOSITY);
        }
        ui32 ttl = Max<ui32>();
        if (traceTTL) {
            ttl = FromStringWithDefault<ui32>(traceTTL, ttl);
            ttl = std::min(ttl, NWilson::TTraceId::MAX_TIME_TO_LIVE);
        }
        traceId = NWilson::TTraceId::NewTraceId(verbosity, ttl);
    }
    if (traceId) {
        Span = {TComponentTracingLevels::THttp::TopLevel, std::move(traceId), handlerName ? "http " + handlerName : "http viewer", NWilson::EFlags::AUTO_END};
        TString uri = request.GetUri();
        Span.Attribute("request_type", TString(TStringBuf(uri).Before('?')));
        Span.Attribute("request_params", TString(TStringBuf(uri).After('?')));
    }
}

TViewerPipeClient::TViewerPipeClient(IViewer* viewer, NMon::TEvHttpInfo::TPtr& ev, const TString& handlerName)
    : Viewer(viewer)
    , Event(ev)
{
    Params = Event->Get()->Request.GetParams();
    if (NHttp::Trim(Event->Get()->Request.GetHeader("Content-Type").Before(';'), ' ') == "application/json") {
        BuildParamsFromJson(Event->Get()->Request.GetPostContent());
    }
    InitConfig(Params);
    SetupTracing(handlerName);
}

TViewerPipeClient::TViewerPipeClient(IViewer* viewer, NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& ev, const TString& handlerName)
    : Viewer(viewer)
    , HttpEvent(ev)
{
    Params = TCgiParameters(HttpEvent->Get()->Request->URL.After('?'));
    NHttp::THeaders headers(HttpEvent->Get()->Request->Headers);
    if (NHttp::Trim(headers.Get("Content-Type").Before(';'), ' ') == "application/json") {
        BuildParamsFromJson(HttpEvent->Get()->Request->Body);
    }
    InitConfig(Params);
    SetupTracing(handlerName);
}

TActorId TViewerPipeClient::ConnectTabletPipe(NNodeWhiteboard::TTabletId tabletId) {
    TPipeInfo& pipeInfo = PipeInfo[tabletId];
    if (!pipeInfo.PipeClient) {
        auto pipe = NTabletPipe::CreateClient(SelfId(), tabletId, GetPipeClientConfig());
        pipeInfo.PipeClient = RegisterWithSameMailbox(pipe);
    }
    pipeInfo.Requests++;
    return pipeInfo.PipeClient;
}

void TViewerPipeClient::SendEvent(std::unique_ptr<IEventHandle> event) {
    if (DelayedRequests.empty() && Requests < MaxRequestsInFlight) {
        TActivationContext::Send(event.release());
        ++Requests;
    } else {
        DelayedRequests.push_back({
            .Event = std::move(event),
        });
    }
}

void TViewerPipeClient::SendRequest(TActorId recipient, IEventBase* ev, ui32 flags, ui64 cookie, NWilson::TTraceId traceId) {
    SendEvent(std::make_unique<IEventHandle>(recipient, SelfId(), ev, flags, cookie, nullptr /*forwardOnNondelivery*/, std::move(traceId)));
}

void TViewerPipeClient::SendRequestToPipe(TActorId pipe, IEventBase* ev, ui64 cookie, NWilson::TTraceId traceId) {
    std::unique_ptr<IEventHandle> event = std::make_unique<IEventHandle>(pipe, SelfId(), ev, 0 /*flags*/, cookie, nullptr /*forwardOnNondelivery*/, std::move(traceId));
    event->Rewrite(TEvTabletPipe::EvSend, pipe);
    SendEvent(std::move(event));
}

void TViewerPipeClient::SendDelayedRequests() {
    while (!DelayedRequests.empty() && Requests < MaxRequestsInFlight) {
        auto& request(DelayedRequests.front());
        TActivationContext::Send(request.Event.release());
        ++Requests;
        DelayedRequests.pop_front();
    }
}

TPathId TViewerPipeClient::GetPathId(const TEvTxProxySchemeCache::TEvNavigateKeySetResult& ev) {
    if (ev.Request->ResultSet.size() == 1) {
        if (ev.Request->ResultSet.begin()->Self) {
            const auto& info = ev.Request->ResultSet.begin()->Self->Info;
            return TPathId(info.GetSchemeshardId(), info.GetPathId());
        }
        if (ev.Request->ResultSet.begin()->TableId) {
            return ev.Request->ResultSet.begin()->TableId.PathId;
        }
    }
    return {};
}

TString TViewerPipeClient::GetPath(const TEvTxProxySchemeCache::TEvNavigateKeySetResult& ev) {
    if (ev.Request->ResultSet.size() == 1) {
        return CanonizePath(ev.Request->ResultSet.begin()->Path);
    }
    return {};
}

TPathId TViewerPipeClient::GetPathId(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
    return GetPathId(*ev->Get());
}

TString TViewerPipeClient::GetPath(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
    return GetPath(*ev->Get());
}

bool TViewerPipeClient::IsSuccess(const std::unique_ptr<TEvTxProxySchemeCache::TEvNavigateKeySetResult>& ev) {
    return (ev->Request->ResultSet.size() > 0) && (std::find_if(ev->Request->ResultSet.begin(), ev->Request->ResultSet.end(),
        [](const auto& entry) {
            return entry.Status == NSchemeCache::TSchemeCacheNavigate::EStatus::Ok;
        }) != ev->Request->ResultSet.end());
}

TString TViewerPipeClient::GetError(const std::unique_ptr<TEvTxProxySchemeCache::TEvNavigateKeySetResult>& ev) {
    if (ev->Request->ResultSet.size() == 0) {
        return "empty response";
    }
    for (const auto& entry : ev->Request->ResultSet) {
        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            switch (entry.Status) {
                case NSchemeCache::TSchemeCacheNavigate::EStatus::Ok:
                    return "Ok";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::Unknown:
                    return "Unknown";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::RootUnknown:
                    return "RootUnknown";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::PathErrorUnknown:
                    return "PathErrorUnknown";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::PathNotTable:
                    return "PathNotTable";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::PathNotPath:
                    return "PathNotPath";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::TableCreationNotComplete:
                    return "TableCreationNotComplete";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError:
                    return "LookupError";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::RedirectLookupError:
                    return "RedirectLookupError";
                case NSchemeCache::TSchemeCacheNavigate::EStatus::AccessDenied:
                    return "AccessDenied";
                default:
                    return ::ToString(static_cast<int>(ev->Request->ResultSet.begin()->Status));
            }
        }
    }
    return "no error";
}

bool TViewerPipeClient::IsSuccess(const std::unique_ptr<TEvStateStorage::TEvBoardInfo>& ev) {
    return ev->Status == TEvStateStorage::TEvBoardInfo::EStatus::Ok;
}

TString TViewerPipeClient::GetError(const std::unique_ptr<TEvStateStorage::TEvBoardInfo>& ev) {
    switch (ev->Status) {
        case TEvStateStorage::TEvBoardInfo::EStatus::Unknown:
            return "Unknown";
        case TEvStateStorage::TEvBoardInfo::EStatus::Ok:
            return "Ok";
        case TEvStateStorage::TEvBoardInfo::EStatus::NotAvailable:
            return "NotAvailable";
        default:
            return ::ToString(static_cast<int>(ev->Status));
    }
}

void TViewerPipeClient::RequestHiveDomainStats(NNodeWhiteboard::TTabletId hiveId) {
    TActorId pipeClient = ConnectTabletPipe(hiveId);
    THolder<TEvHive::TEvRequestHiveDomainStats> request = MakeHolder<TEvHive::TEvRequestHiveDomainStats>();
    request->Record.SetReturnFollowers(Followers);
    request->Record.SetReturnMetrics(Metrics);
    SendRequestToPipe(pipeClient, request.Release(), hiveId);
}

void TViewerPipeClient::RequestHiveNodeStats(NNodeWhiteboard::TTabletId hiveId, TPathId pathId) {
    TActorId pipeClient = ConnectTabletPipe(hiveId);
    THolder<TEvHive::TEvRequestHiveNodeStats> request = MakeHolder<TEvHive::TEvRequestHiveNodeStats>();
    request->Record.SetReturnMetrics(Metrics);
    if (pathId != TPathId()) {
        request->Record.SetReturnExtendedTabletInfo(true);
        request->Record.SetFilterTabletsBySchemeShardId(pathId.OwnerId);
        request->Record.SetFilterTabletsByPathId(pathId.LocalPathId);
    }
    SendRequestToPipe(pipeClient, request.Release(), hiveId);
}

void TViewerPipeClient::RequestHiveStorageStats(NNodeWhiteboard::TTabletId hiveId) {
    TActorId pipeClient = ConnectTabletPipe(hiveId);
    THolder<TEvHive::TEvRequestHiveStorageStats> request = MakeHolder<TEvHive::TEvRequestHiveStorageStats>();
    SendRequestToPipe(pipeClient, request.Release(), hiveId);
}

TViewerPipeClient::TRequestResponse<TEvViewer::TEvViewerResponse> TViewerPipeClient::MakeViewerRequest(TNodeId nodeId, TEvViewer::TEvViewerRequest* ev, ui32 flags) {
    TActorId viewerServiceId = MakeViewerID(nodeId);
    TRequestResponse<TEvViewer::TEvViewerResponse> response(Span.CreateChild(TComponentTracingLevels::THttp::Detailed, TypeName(*ev)));
    if (response.Span) {
        response.Span.Attribute("target_node_id", nodeId);
        TStringBuilder askFor;
        askFor << ev->Record.GetLocation().NodeIdSize() << " nodes (";
        for (size_t i = 0; i < std::min<size_t>(ev->Record.GetLocation().NodeIdSize(), 16); ++i) {
            if (i) {
                askFor << ", ";
            }
            askFor << ev->Record.GetLocation().GetNodeId(i);
        }
        if (ev->Record.GetLocation().NodeIdSize() > 16) {
            askFor << ", ...";
        }
        askFor << ")";
        response.Span.Attribute("ask_for", askFor);
        switch (ev->Record.Request_case()) {
            case NKikimrViewer::TEvViewerRequest::kTabletRequest:
                response.Span.Attribute("request_type", "TabletRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kSystemRequest:
                response.Span.Attribute("request_type", "SystemRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kPDiskRequest:
                response.Span.Attribute("request_type", "PDiskRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kVDiskRequest:
                response.Span.Attribute("request_type", "VDiskRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kNodeRequest:
                response.Span.Attribute("request_type", "NodeRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kQueryRequest:
                response.Span.Attribute("request_type", "QueryRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kRenderRequest:
                response.Span.Attribute("request_type", "RenderRequest");
                break;
            case NKikimrViewer::TEvViewerRequest::kAutocompleteRequest:
                response.Span.Attribute("request_type", "AutocompleteRequest");
                break;
            default:
                response.Span.Attribute("request_type", ::ToString(static_cast<int>(ev->Record.Request_case())));
                break;
        }
    }
    SendRequest(viewerServiceId, ev, flags, nodeId, response.Span.GetTraceId());
    return response;
}

TViewerPipeClient::TRequestResponse<TEvHive::TEvResponseHiveDomainStats> TViewerPipeClient::MakeRequestHiveDomainStats(NNodeWhiteboard::TTabletId hiveId) {
    TActorId pipeClient = ConnectTabletPipe(hiveId);
    THolder<TEvHive::TEvRequestHiveDomainStats> request = MakeHolder<TEvHive::TEvRequestHiveDomainStats>();
    request->Record.SetReturnFollowers(Followers);
    request->Record.SetReturnMetrics(Metrics);
    auto response = MakeRequestToPipe<TEvHive::TEvResponseHiveDomainStats>(pipeClient, request.Release(), hiveId);
    if (response.Span) {
        auto hive_id = "#" + ::ToString(hiveId);
        response.Span.Attribute("hive_id", hive_id);
    }
    return response;
}

TViewerPipeClient::TRequestResponse<TEvHive::TEvResponseHiveStorageStats> TViewerPipeClient::MakeRequestHiveStorageStats(NNodeWhiteboard::TTabletId hiveId) {
    TActorId pipeClient = ConnectTabletPipe(hiveId);
    THolder<TEvHive::TEvRequestHiveStorageStats> request = MakeHolder<TEvHive::TEvRequestHiveStorageStats>();
    auto response = MakeRequestToPipe<TEvHive::TEvResponseHiveStorageStats>(pipeClient, request.Release(), hiveId);
    if (response.Span) {
        auto hive_id = "#" + ::ToString(hiveId);
        response.Span.Attribute("hive_id", hive_id);
    }
    return response;
}

TViewerPipeClient::TRequestResponse<TEvHive::TEvResponseHiveNodeStats> TViewerPipeClient::MakeRequestHiveNodeStats(TTabletId hiveId, TEvHive::TEvRequestHiveNodeStats* request) {
    TActorId pipeClient = ConnectTabletPipe(hiveId);
    auto response = MakeRequestToPipe<TEvHive::TEvResponseHiveNodeStats>(pipeClient, request, hiveId);
    if (response.Span) {
        auto hive_id = "#" + ::ToString(hiveId);
        response.Span.Attribute("hive_id", hive_id);
    }
    return response;
}

TViewerPipeClient::TRequestResponse<TEvViewer::TEvViewerResponse> TViewerPipeClient::MakeRequestViewer(TNodeId nodeId, TEvViewer::TEvViewerRequest* request, ui32 flags) {
    auto requestType = request->Record.GetRequestCase();
    auto response = MakeRequest<TEvViewer::TEvViewerResponse>(MakeViewerID(nodeId), request, flags, nodeId);
    if (response.Span) {
        TString requestTypeString;
        switch (requestType) {
            case NKikimrViewer::TEvViewerRequest::kTabletRequest:
                requestTypeString = "TabletRequest";
                break;
            case NKikimrViewer::TEvViewerRequest::kSystemRequest:
                requestTypeString = "SystemRequest";
                break;
            case NKikimrViewer::TEvViewerRequest::kQueryRequest:
                requestTypeString = "QueryRequest";
                break;
            case NKikimrViewer::TEvViewerRequest::kRenderRequest:
                requestTypeString = "RenderRequest";
                break;
            case NKikimrViewer::TEvViewerRequest::kAutocompleteRequest:
                requestTypeString = "AutocompleteRequest";
                break;
            default:
                requestTypeString = ::ToString(static_cast<int>(requestType));
                break;
        }
        response.Span.Attribute("request_type", requestTypeString);
    }
    return response;
}

void TViewerPipeClient::RequestConsoleListTenants() {
    TActorId pipeClient = ConnectTabletPipe(GetConsoleId());
    THolder<NConsole::TEvConsole::TEvListTenantsRequest> request = MakeHolder<NConsole::TEvConsole::TEvListTenantsRequest>();
    SendRequestToPipe(pipeClient, request.Release());
}

TViewerPipeClient::TRequestResponse<NConsole::TEvConsole::TEvListTenantsResponse> TViewerPipeClient::MakeRequestConsoleListTenants() {
    TActorId pipeClient = ConnectTabletPipe(GetConsoleId());
    THolder<NConsole::TEvConsole::TEvListTenantsRequest> request = MakeHolder<NConsole::TEvConsole::TEvListTenantsRequest>();
    return MakeRequestToPipe<NConsole::TEvConsole::TEvListTenantsResponse>(pipeClient, request.Release());
}

TViewerPipeClient::TRequestResponse<NConsole::TEvConsole::TEvGetNodeConfigResponse> TViewerPipeClient::MakeRequestConsoleNodeConfigByTenant(TString tenant, ui64 cookie) {
    TActorId pipeClient = ConnectTabletPipe(GetConsoleId());
    auto request = MakeHolder<NConsole::TEvConsole::TEvGetNodeConfigRequest>();
    request->Record.MutableNode()->SetTenant(tenant);
    request->Record.AddItemKinds(static_cast<ui32>(NKikimrConsole::TConfigItem::FeatureFlagsItem));
    return MakeRequestToPipe<NConsole::TEvConsole::TEvGetNodeConfigResponse>(pipeClient, request.Release(), cookie);
}

TViewerPipeClient::TRequestResponse<NConsole::TEvConsole::TEvGetAllConfigsResponse> TViewerPipeClient::MakeRequestConsoleGetAllConfigs() {
    TActorId pipeClient = ConnectTabletPipe(GetConsoleId());
    return MakeRequestToPipe<NConsole::TEvConsole::TEvGetAllConfigsResponse>(pipeClient, new NConsole::TEvConsole::TEvGetAllConfigsRequest());
}

void TViewerPipeClient::RequestConsoleGetTenantStatus(const TString& path) {
    TActorId pipeClient = ConnectTabletPipe(GetConsoleId());
    THolder<NConsole::TEvConsole::TEvGetTenantStatusRequest> request = MakeHolder<NConsole::TEvConsole::TEvGetTenantStatusRequest>();
    request->Record.MutableRequest()->set_path(path);
    SendRequestToPipe(pipeClient, request.Release());
}

TViewerPipeClient::TRequestResponse<NConsole::TEvConsole::TEvGetTenantStatusResponse> TViewerPipeClient::MakeRequestConsoleGetTenantStatus(const TString& path) {
    TActorId pipeClient = ConnectTabletPipe(GetConsoleId());
    THolder<NConsole::TEvConsole::TEvGetTenantStatusRequest> request = MakeHolder<NConsole::TEvConsole::TEvGetTenantStatusRequest>();
    request->Record.MutableRequest()->set_path(path);
    auto response = MakeRequestToPipe<NConsole::TEvConsole::TEvGetTenantStatusResponse>(pipeClient, request.Release());
    if (response.Span) {
        response.Span.Attribute("path", path);
    }
    return response;
}

void TViewerPipeClient::RequestBSControllerConfig() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvControllerConfigRequest> request = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
    request->Record.MutableRequest()->AddCommand()->MutableQueryBaseConfig();
    SendRequestToPipe(pipeClient, request.Release());
}

void TViewerPipeClient::RequestBSControllerConfigWithStoragePools() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvControllerConfigRequest> request = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
    request->Record.MutableRequest()->AddCommand()->MutableQueryBaseConfig();
    request->Record.MutableRequest()->AddCommand()->MutableReadStoragePool()->SetBoxId(Max<ui64>());
    SendRequestToPipe(pipeClient, request.Release());
}

TViewerPipeClient::TRequestResponse<TEvBlobStorage::TEvControllerConfigResponse> TViewerPipeClient::MakeRequestBSControllerConfigWithStoragePools() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvControllerConfigRequest> request = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
    request->Record.MutableRequest()->AddCommand()->MutableQueryBaseConfig();
    request->Record.MutableRequest()->AddCommand()->MutableReadStoragePool()->SetBoxId(Max<ui64>());
    return MakeRequestToPipe<TEvBlobStorage::TEvControllerConfigResponse>(pipeClient, request.Release());
}

void TViewerPipeClient::RequestBSControllerInfo() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvRequestControllerInfo> request = MakeHolder<TEvBlobStorage::TEvRequestControllerInfo>();
    SendRequestToPipe(pipeClient, request.Release());
}

void TViewerPipeClient::RequestBSControllerSelectGroups(THolder<TEvBlobStorage::TEvControllerSelectGroups> request) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    SendRequestToPipe(pipeClient, request.Release());
}

TViewerPipeClient::TRequestResponse<TEvBlobStorage::TEvControllerSelectGroupsResult> TViewerPipeClient::MakeRequestBSControllerSelectGroups(THolder<TEvBlobStorage::TEvControllerSelectGroups> request, ui64 cookie) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    return MakeRequestToPipe<TEvBlobStorage::TEvControllerSelectGroupsResult>(pipeClient, request.Release(), cookie);
}

void TViewerPipeClient::RequestBSControllerPDiskRestart(ui32 nodeId, ui32 pdiskId, bool force) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvControllerConfigRequest> request = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
    auto* restartPDisk = request->Record.MutableRequest()->AddCommand()->MutableRestartPDisk();
    restartPDisk->MutableTargetPDiskId()->SetNodeId(nodeId);
    restartPDisk->MutableTargetPDiskId()->SetPDiskId(pdiskId);
    if (force) {
        request->Record.MutableRequest()->SetIgnoreDegradedGroupsChecks(true);
    }
    SendRequestToPipe(pipeClient, request.Release());
}

void TViewerPipeClient::RequestBSControllerVDiskEvict(ui32 groupId, ui32 groupGeneration, ui32 failRealmIdx, ui32 failDomainIdx, ui32 vdiskIdx, bool force) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvControllerConfigRequest> request = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
    auto* evictVDisk = request->Record.MutableRequest()->AddCommand()->MutableReassignGroupDisk();
    evictVDisk->SetGroupId(groupId);
    evictVDisk->SetGroupGeneration(groupGeneration);
    evictVDisk->SetFailRealmIdx(failRealmIdx);
    evictVDisk->SetFailDomainIdx(failDomainIdx);
    evictVDisk->SetVDiskIdx(vdiskIdx);
    if (force) {
        request->Record.MutableRequest()->SetIgnoreDegradedGroupsChecks(true);
    }
    SendRequestToPipe(pipeClient, request.Release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetPDisksResponse> TViewerPipeClient::RequestBSControllerPDiskInfo(ui32 nodeId, ui32 pdiskId) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    auto request = std::make_unique<NSysView::TEvSysView::TEvGetPDisksRequest>();
    request->Record.SetInclusiveFrom(true);
    request->Record.SetInclusiveTo(true);
    request->Record.MutableFrom()->SetNodeId(nodeId);
    request->Record.MutableFrom()->SetPDiskId(pdiskId);
    request->Record.MutableTo()->SetNodeId(nodeId);
    request->Record.MutableTo()->SetPDiskId(pdiskId);
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetPDisksResponse>(pipeClient, request.release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetVSlotsResponse> TViewerPipeClient::RequestBSControllerVDiskInfo(ui32 nodeId, ui32 pdiskId) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    auto request = std::make_unique<NSysView::TEvSysView::TEvGetVSlotsRequest>();
    request->Record.SetInclusiveFrom(true);
    request->Record.SetInclusiveTo(true);
    request->Record.MutableFrom()->SetNodeId(nodeId);
    request->Record.MutableFrom()->SetPDiskId(pdiskId);
    request->Record.MutableFrom()->SetVSlotId(0);
    request->Record.MutableTo()->SetNodeId(nodeId);
    request->Record.MutableTo()->SetPDiskId(pdiskId);
    request->Record.MutableTo()->SetVSlotId(std::numeric_limits<ui32>::max());
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetVSlotsResponse>(pipeClient, request.release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetGroupsResponse> TViewerPipeClient::RequestBSControllerGroups() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    auto request = std::make_unique<NSysView::TEvSysView::TEvGetGroupsRequest>();
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetGroupsResponse>(pipeClient, request.release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetStoragePoolsResponse> TViewerPipeClient::RequestBSControllerPools() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    auto request = std::make_unique<NSysView::TEvSysView::TEvGetStoragePoolsRequest>();
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetStoragePoolsResponse>(pipeClient, request.release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetVSlotsResponse> TViewerPipeClient::RequestBSControllerVSlots() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    auto request = std::make_unique<NSysView::TEvSysView::TEvGetVSlotsRequest>();
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetVSlotsResponse>(pipeClient, request.release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetPDisksResponse> TViewerPipeClient::RequestBSControllerPDisks() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    auto request = std::make_unique<NSysView::TEvSysView::TEvGetPDisksRequest>();
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetPDisksResponse>(pipeClient, request.release());
}

TViewerPipeClient::TRequestResponse<NSysView::TEvSysView::TEvGetStorageStatsResponse> TViewerPipeClient::RequestBSControllerStorageStats() {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    return MakeRequestToPipe<NSysView::TEvSysView::TEvGetStorageStatsResponse>(pipeClient, new NSysView::TEvSysView::TEvGetStorageStatsRequest());
}

void TViewerPipeClient::RequestBSControllerPDiskUpdateStatus(const NKikimrBlobStorage::TUpdateDriveStatus& driveStatus, bool force) {
    TActorId pipeClient = ConnectTabletPipe(GetBSControllerId());
    THolder<TEvBlobStorage::TEvControllerConfigRequest> request = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
    auto* updateDriveStatus = request->Record.MutableRequest()->AddCommand()->MutableUpdateDriveStatus();
    updateDriveStatus->CopyFrom(driveStatus);
    if (force) {
        request->Record.MutableRequest()->SetIgnoreDegradedGroupsChecks(true);
    }
    SendRequestToPipe(pipeClient, request.Release());
}

void TViewerPipeClient::RequestSchemeCacheNavigate(const TString& path) {
    THolder<NSchemeCache::TSchemeCacheNavigate> request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
    NSchemeCache::TSchemeCacheNavigate::TEntry entry;
    entry.Path = SplitPath(path);
    entry.RedirectRequired = false;
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpPath;
    request->ResultSet.emplace_back(entry);
    SendRequest(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
}

void TViewerPipeClient::RequestSchemeCacheNavigate(const TPathId& pathId) {
    THolder<NSchemeCache::TSchemeCacheNavigate> request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
    NSchemeCache::TSchemeCacheNavigate::TEntry entry;
    entry.TableId.PathId = pathId;
    entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByTableId;
    entry.RedirectRequired = false;
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpPath;
    request->ResultSet.emplace_back(entry);
    SendRequest(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
}

TViewerPipeClient::TRequestResponse<TEvTxProxySchemeCache::TEvNavigateKeySetResult> TViewerPipeClient::MakeRequestSchemeCacheNavigate(const TString& path, ui64 cookie) {
    THolder<NSchemeCache::TSchemeCacheNavigate> request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
    NSchemeCache::TSchemeCacheNavigate::TEntry entry;
    entry.Path = SplitPath(path);
    entry.RedirectRequired = false;
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpPath;
    request->ResultSet.emplace_back(entry);
    auto response = MakeRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()), 0 /*flags*/, cookie);
    if (response.Span) {
        response.Span.Attribute("path", path);
    }
    return response;
}

TViewerPipeClient::TRequestResponse<TEvTxProxySchemeCache::TEvNavigateKeySetResult> TViewerPipeClient::MakeRequestSchemeCacheNavigate(TPathId pathId, ui64 cookie) {
    THolder<NSchemeCache::TSchemeCacheNavigate> request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
    NSchemeCache::TSchemeCacheNavigate::TEntry entry;
    entry.TableId.PathId = pathId;
    entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByTableId;
    entry.RedirectRequired = false;
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpPath;
    request->ResultSet.emplace_back(entry);
    auto response = MakeRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()), 0 /*flags*/, cookie);
    if (response.Span) {
        response.Span.Attribute("path_id", pathId.ToString());
    }
    return response;
}

void TViewerPipeClient::RequestTxProxyDescribe(const TString& path) {
    THolder<TEvTxUserProxy::TEvNavigate> request(new TEvTxUserProxy::TEvNavigate());
    request->Record.MutableDescribePath()->SetPath(path);
    if (Event && !Event->Get()->UserToken.empty()) {
        request->Record.SetUserToken(Event->Get()->UserToken);
    }
    if (HttpEvent && !HttpEvent->Get()->UserToken.empty()) {
        request->Record.SetUserToken(HttpEvent->Get()->UserToken);
    }
    SendRequest(MakeTxProxyID(), request.Release());
}

void TViewerPipeClient::RequestStateStorageEndpointsLookup(const TString& path) {
    RegisterWithSameMailbox(CreateBoardLookupActor(MakeEndpointsBoardPath(path),
                                                   SelfId(),
                                                   EBoardLookupMode::Second));
    ++Requests;
}

TViewerPipeClient::TRequestResponse<TEvStateStorage::TEvBoardInfo> TViewerPipeClient::MakeRequestStateStorageEndpointsLookup(const TString& path, ui64 cookie) {
    TRequestResponse<TEvStateStorage::TEvBoardInfo> response(Span.CreateChild(TComponentTracingLevels::THttp::Detailed, "BoardLookupActor"));
    RegisterWithSameMailbox(CreateBoardLookupActor(MakeEndpointsBoardPath(path),
                                                   SelfId(),
                                                   EBoardLookupMode::Second, {}, cookie));
    if (response.Span) {
        response.Span.Attribute("path", path);
    }
    ++Requests;
    return response;
}

void TViewerPipeClient::RequestStateStorageMetadataCacheEndpointsLookup(const TString& path) {
    if (!AppData()->DomainsInfo->Domain) {
        return;
    }
    RegisterWithSameMailbox(CreateBoardLookupActor(MakeDatabaseMetadataCacheBoardPath(path),
                                                   SelfId(),
                                                   EBoardLookupMode::Second));
    ++Requests;
}

std::vector<TNodeId> TViewerPipeClient::GetNodesFromBoardReply(const TEvStateStorage::TEvBoardInfo& ev) {
    std::vector<TNodeId> databaseNodes;
    if (ev.Status == TEvStateStorage::TEvBoardInfo::EStatus::Ok) {
        for (const auto& [actorId, infoEntry] : ev.InfoEntries) {
            databaseNodes.emplace_back(actorId.NodeId());
        }
    }
    std::sort(databaseNodes.begin(), databaseNodes.end());
    databaseNodes.erase(std::unique(databaseNodes.begin(), databaseNodes.end()), databaseNodes.end());
    return databaseNodes;
}

std::vector<TNodeId> TViewerPipeClient::GetNodesFromBoardReply(TEvStateStorage::TEvBoardInfo::TPtr& ev) {
    return GetNodesFromBoardReply(*ev->Get());
}

void TViewerPipeClient::InitConfig(const TCgiParameters& params) {
    Followers = FromStringWithDefault(params.Get("followers"), Followers);
    Metrics = FromStringWithDefault(params.Get("metrics"), Metrics);
    WithRetry = FromStringWithDefault(params.Get("with_retry"), WithRetry);
    MaxRequestsInFlight = FromStringWithDefault(params.Get("max_requests_in_flight"), MaxRequestsInFlight);
    Database = params.Get("database");
    if (!Database) {
        Database = params.Get("tenant");
    }
    Direct = FromStringWithDefault<bool>(params.Get("direct"), Direct);
    JsonSettings.EnumAsNumbers = !FromStringWithDefault<bool>(params.Get("enums"), true);
    JsonSettings.UI64AsString = !FromStringWithDefault<bool>(params.Get("ui64"), false);
    if (FromStringWithDefault<bool>(params.Get("enums"), true)) {
        Proto2JsonConfig.EnumMode = TProto2JsonConfig::EnumValueMode::EnumName;
    }
    if (!FromStringWithDefault<bool>(params.Get("ui64"), false)) {
        Proto2JsonConfig.StringifyNumbers = TProto2JsonConfig::EStringifyNumbersMode::StringifyInt64Always;
    }
    Proto2JsonConfig.MapAsObject = true;
    Proto2JsonConfig.ConvertAny = true;
    Proto2JsonConfig.WriteNanAsString = true;
    Timeout = TDuration::MilliSeconds(FromStringWithDefault<ui32>(params.Get("timeout"), Timeout.MilliSeconds()));
}

void TViewerPipeClient::InitConfig(const TRequestSettings& settings) {
    Followers = settings.Followers;
    Metrics = settings.Metrics;
    WithRetry = settings.WithRetry;
}

void TViewerPipeClient::ClosePipes() {
    for (const auto& [tabletId, pipeInfo] : PipeInfo) {
        if (pipeInfo.PipeClient) {
            NTabletPipe::CloseClient(SelfId(), pipeInfo.PipeClient);
        }
    }
    PipeInfo.clear();
}

ui32 TViewerPipeClient::FailPipeConnect(NNodeWhiteboard::TTabletId tabletId) {
    auto itPipeInfo = PipeInfo.find(tabletId);
    if (itPipeInfo != PipeInfo.end()) {
        ui32 requests = itPipeInfo->second.Requests;
        NTabletPipe::CloseClient(SelfId(), itPipeInfo->second.PipeClient);
        PipeInfo.erase(itPipeInfo);
        return requests;
    }
    return 0;
}

TRequestState TViewerPipeClient::GetRequest() const {
    if (Event) {
        return {Event->Get(), Span.GetTraceId()};
    } else if (HttpEvent) {
        return {HttpEvent->Get(), Span.GetTraceId()};
    }
    return {};
}

void TViewerPipeClient::ReplyAndPassAway(TString data, const TString& error) {
    TString message = error;

    if (Event) {
        Send(Event->Sender, new NMon::TEvHttpInfoRes(data, 0, NMon::IEvHttpInfoRes::EContentType::Custom));
    } else if (HttpEvent) {
        auto response = HttpEvent->Get()->Request->CreateResponseString(data);
        Send(HttpEvent->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(response.Release()));
    }

    if (message.empty()) {
        TStringBuf dataParser(data);
        if (dataParser.NextTok(' ') == "HTTP/1.1") {
            TStringBuf code = dataParser.NextTok(' ');
            if (code.size() == 3 && code[0] != '2') {
                message = dataParser.NextTok('\n');
            }
        }
    }
    if (Span) {
        if (message) {
            Span.EndError(message);
        } else {
            Span.EndOk();
        }
    }
    PassAway();
}

TString TViewerPipeClient::GetHTTPOK(TString contentType, TString response, TInstant lastModified) {
    return Viewer->GetHTTPOK(GetRequest(), std::move(contentType), std::move(response), lastModified);
}

TString TViewerPipeClient::GetHTTPOKJSON(TString response, TInstant lastModified) {
    return Viewer->GetHTTPOKJSON(GetRequest(), std::move(response), lastModified);
}

TString TViewerPipeClient::GetHTTPOKJSON(const NJson::TJsonValue& response, TInstant lastModified) {
    constexpr ui32 doubleNDigits = std::numeric_limits<double>::max_digits10;
    constexpr ui32 floatNDigits = std::numeric_limits<float>::max_digits10;
    constexpr EFloatToStringMode floatMode = EFloatToStringMode::PREC_NDIGITS;
    TStringStream content;
    NJson::WriteJson(&content, &response, {
        .DoubleNDigits = doubleNDigits,
        .FloatNDigits = floatNDigits,
        .FloatToStringMode = floatMode,
        .ValidateUtf8 = false,
        .WriteNanAsString = true,
    });
    return GetHTTPOKJSON(content.Str(), lastModified);
}

TString TViewerPipeClient::GetHTTPOKJSON(const google::protobuf::Message& response, TInstant lastModified) {
    TStringStream json;
    NProtobufJson::Proto2Json(response, json, Proto2JsonConfig);
    return GetHTTPOKJSON(json.Str(), lastModified);
}

TString TViewerPipeClient::GetHTTPGATEWAYTIMEOUT(TString contentType, TString response) {
    return Viewer->GetHTTPGATEWAYTIMEOUT(GetRequest(), std::move(contentType), std::move(response));
}

TString TViewerPipeClient::GetHTTPBADREQUEST(TString contentType, TString response) {
    return Viewer->GetHTTPBADREQUEST(GetRequest(), std::move(contentType), std::move(response));
}

TString TViewerPipeClient::GetHTTPINTERNALERROR(TString contentType, TString response) {
    return Viewer->GetHTTPINTERNALERROR(GetRequest(), std::move(contentType), std::move(response));
}

TString TViewerPipeClient::GetHTTPFORBIDDEN(TString contentType, TString response) {
    return Viewer->GetHTTPFORBIDDEN(GetRequest(), std::move(contentType), std::move(response));
}

TString TViewerPipeClient::MakeForward(const std::vector<ui32>& nodes) {
    return Viewer->MakeForward(GetRequest(), nodes);
}

void TViewerPipeClient::RequestDone(ui32 requests) {
    if (requests == 0) {
        return;
    }
    if (requests > Requests) {
        BLOG_ERROR("Requests count mismatch: " << requests << " > " << Requests);
        if (Span) {
            Span.Event("Requests count mismatch");
        }
        requests = Requests;
    }
    Requests -= requests;
    if (!DelayedRequests.empty()) {
        SendDelayedRequests();
    }
    if (Requests == 0 && !PassedAway) {
        ReplyAndPassAway();
    }
}

void TViewerPipeClient::CancelAllRequests() {
    DelayedRequests.clear();
}

void TViewerPipeClient::Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev) {
    if (ev->Get()->Status != NKikimrProto::OK) {
        ui32 requests = FailPipeConnect(ev->Get()->TabletId);
        RequestDone(requests);
    }
}

void TViewerPipeClient::HandleResolveResource(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
    if (ResourceNavigateResponse) {
        ResourceNavigateResponse->Set(std::move(ev));
        if (ResourceNavigateResponse->IsOk()) {
            TSchemeCacheNavigate::TEntry& entry(ResourceNavigateResponse->Get()->Request->ResultSet.front());
            SharedDatabase = CanonizePath(entry.Path);
            Direct |= (SharedDatabase == AppData()->TenantName);
            DatabaseBoardInfoResponse = MakeRequestStateStorageEndpointsLookup(SharedDatabase);
            --Requests; // don't count this request
        } else {
            AddEvent("Failed to resolve database - shared database not found");
            Direct = true;
            Bootstrap(); // retry bootstrap without redirect this time
        }
    }
}

void TViewerPipeClient::HandleResolveDatabase(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
    if (DatabaseNavigateResponse) {
        DatabaseNavigateResponse->Set(std::move(ev));
        if (DatabaseNavigateResponse->IsOk()) {
            TSchemeCacheNavigate::TEntry& entry(DatabaseNavigateResponse->Get()->Request->ResultSet.front());
            if (entry.DomainInfo && entry.DomainInfo->ResourcesDomainKey && entry.DomainInfo->DomainKey != entry.DomainInfo->ResourcesDomainKey) {
                ResourceNavigateResponse = MakeRequestSchemeCacheNavigate(TPathId(entry.DomainInfo->ResourcesDomainKey));
                --Requests; // don't count this request
                Become(&TViewerPipeClient::StateResolveResource);
            } else {
                Database = CanonizePath(entry.Path);
                DatabaseBoardInfoResponse = MakeRequestStateStorageEndpointsLookup(Database);
                --Requests; // don't count this request
            }
        } else {
            AddEvent("Failed to resolve database - not found");
            Direct = true;
            Bootstrap(); // retry bootstrap without redirect this time
        }
    }
}

void TViewerPipeClient::HandleResolve(TEvStateStorage::TEvBoardInfo::TPtr& ev) {
    if (DatabaseBoardInfoResponse) {
        DatabaseBoardInfoResponse->Set(std::move(ev));
        if (DatabaseBoardInfoResponse->IsOk()) {
            if (Direct) {
                Bootstrap(); // retry bootstrap without redirect this time
            } else {
                ReplyAndPassAway(MakeForward(GetNodesFromBoardReply(DatabaseBoardInfoResponse->GetRef())));
            }
        } else {
            AddEvent("Failed to resolve database nodes");
            Direct = true;
            Bootstrap(); // retry bootstrap without redirect this time
        }
    }
}

void TViewerPipeClient::HandleTimeout() {
    ReplyAndPassAway(GetHTTPGATEWAYTIMEOUT());
}

STATEFN(TViewerPipeClient::StateResolveDatabase) {
    switch (ev->GetTypeRewrite()) {
        hFunc(TEvStateStorage::TEvBoardInfo, HandleResolve);
        hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, HandleResolveDatabase);
        cFunc(TEvents::TEvWakeup::EventType, HandleTimeout);
    }
}

STATEFN(TViewerPipeClient::StateResolveResource) {
    switch (ev->GetTypeRewrite()) {
        hFunc(TEvStateStorage::TEvBoardInfo, HandleResolve);
        hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, HandleResolveResource);
        cFunc(TEvents::TEvWakeup::EventType, HandleTimeout);
    }
}

void TViewerPipeClient::RedirectToDatabase(const TString& database) {
    DatabaseNavigateResponse = MakeRequestSchemeCacheNavigate(database);
    --Requests; // don't count this request
    Become(&TViewerPipeClient::StateResolveDatabase);
}

bool TViewerPipeClient::NeedToRedirect() {
    auto request = GetRequest();
    if (NeedRedirect && request) {
        NeedRedirect = false;
        Direct |= !request.GetHeader("X-Forwarded-From-Node").empty(); // we're already forwarding
        Direct |= (Database == AppData()->TenantName) || Database.empty(); // we're already on the right node or don't use database filter
        if (Database) {
            RedirectToDatabase(Database); // to find some dynamic node and redirect query there
            return true;
        }
    }
    return false;
}

void TViewerPipeClient::PassAway() {
    if (Span) {
        Span.EndError("unterminated span");
    }
    std::sort(SubscriptionNodeIds.begin(), SubscriptionNodeIds.end());
    SubscriptionNodeIds.erase(std::unique(SubscriptionNodeIds.begin(), SubscriptionNodeIds.end()), SubscriptionNodeIds.end());
    for (TNodeId nodeId : SubscriptionNodeIds) {
        Send(TActivationContext::InterconnectProxy(nodeId), new TEvents::TEvUnsubscribe());
    }
    ClosePipes();
    CancelAllRequests();
    PassedAway = true;
    TBase::PassAway();
}

void TViewerPipeClient::AddEvent(const TString& name) {
    if (Span) {
        Span.Event(name);
    }
}

}
