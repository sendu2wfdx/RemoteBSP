"use strict";
(() => {
  const MAX_NODES = 128, MAX_RESOURCES = 256, REFRESH_MS = 2000, RETRY_STREAM_MS = 10000, MAX_STREAM_BUFFER = 1024 * 1024;
  const $ = id => document.getElementById(id);
  const state = {busy:false, timer:null, stream:null, generation:0,busResetPermitted:false,busLeases:new Map(),unknownBusOperations:new Map()};
  const text = (tag, value, cls) => { const e=document.createElement(tag); e.textContent=String(value); if(cls)e.className=cls; return e; };
  const chip = value => text("span", value ?? "unknown", `chip ${value ?? "unknown"}`);
  function kv(value) {
    const list=document.createElement("dl"); list.className="kv";
    Object.entries(value || {}).slice(0,32).forEach(([key,item]) => {
      list.append(text("dt",key),text("dd",item === null ? "unknown" : typeof item === "object" ? JSON.stringify(item) : item));
    }); return list;
  }
  function trend(samples) {
    const svg=document.createElementNS("http://www.w3.org/2000/svg","svg"); svg.classList.add("trend"); svg.setAttribute("viewBox","0 0 240 58"); svg.setAttribute("role","img"); svg.setAttribute("aria-label","最近趋势");
    const values=(samples || []).slice(-60).map(s => Object.values(s.values || {}).find(v => Number.isInteger(v))).filter(Number.isFinite);
    if(values.length < 2) return svg;
    const lo=Math.min(...values), hi=Math.max(...values), span=Math.max(1,hi-lo);
    const points=values.map((v,i)=>`${i*240/(values.length-1)},${54-(v-lo)*50/span}`).join(" ");
    const line=document.createElementNS(svg.namespaceURI,"polyline"); line.setAttribute("points",points); svg.append(line); return svg;
  }
  function renderAlerts(alerts) {
    const ul=document.createElement("ul"); ul.className="alerts";
    (alerts || []).slice(0,32).forEach(a => ul.append(text("li",`${a.severity || "unknown"} · ${a.message || a.code || a.metric || "未命名告警"}`)));
    return ul;
  }
  const headers = () => { const value={"Accept":"application/json"},key=$("api-key").value;if(key)value["X-API-Key"]=key;return value; };
  const post = async (path, body) => { const response=await fetch(path,{method:"POST",headers:{...headers(),"Content-Type":"application/json"},body:JSON.stringify(body),credentials:"same-origin"});const value=await response.json();return {response,value}; };
  const operationKey = (node,resource) => `${node.node_id}/${resource.resource_id}`;
  async function acquireBusLease(node,resource) {
    const key=operationKey(node,resource),idem=`web-bus-lease-${crypto.randomUUID()}`;
    const {response,value}=await post("/api/v1/control-leases",{node_id:node.node_id,resource_id:resource.resource_id,command_group:"bus.reset",ttl_ms:30000,idempotency_key:idem});
    if(!response.ok)throw new Error(value.error?.message || `HTTP ${response.status}`);
    state.busLeases.set(key,value.data.lease.lease_id);render(state.lastOverview);
  }
  async function resetBus(node,resource) {
    const key=operationKey(node,resource),lease=state.busLeases.get(key);if(!lease||!confirm(`确认复位 ${resource.display_name || resource.resource_id}？`))return;
    const idem=`web-bus-reset-${crypto.randomUUID()}`,{response,value}=await post("/api/v1/control/bus/reset",{lease_id:lease,node_id:node.node_id,resource_id:resource.resource_id,idempotency_key:idem});
    const operation=response.ok?value.data?.operation:value.error?.details?.operation;
    if(operation?.state==="unknown"){state.unknownBusOperations.set(key,{location:response.headers.get("Location"),operation});state.busLeases.delete(key);render(state.lastOverview);return;}
    if(!response.ok)throw new Error(value.error?.message || `HTTP ${response.status}`);
    state.busLeases.delete(key);render(state.lastOverview);
  }
  async function queryBusOperation(key) {
    const pending=state.unknownBusOperations.get(key);if(!pending?.location)return;
    const response=await fetch(pending.location,{headers:headers(),cache:"no-store"}),value=await response.json();
    const operation=response.ok?value.data?.operation:value.error?.details?.operation;
    if(operation){if(operation.state==="unknown"||operation.state==="pending")state.unknownBusOperations.set(key,{...pending,operation});else state.unknownBusOperations.delete(key);}render(state.lastOverview);
  }
  function renderResource(node,resource) {
    const item=document.createElement("details"); item.className="resource";
    const summary=document.createElement("summary"); summary.append(text("span",resource.display_name || resource.resource_id || "未命名资源"));
    const chips=text("span","","chips"); chips.append(chip(resource.availability),chip(resource.health)); summary.append(chips); item.append(summary);
    item.append(kv({resource_id:resource.resource_id,kind:resource.kind,...(resource.state || {})}));
    if(resource.bus_health){const h=resource.bus_health;item.append(text("h4","总线健康"),kv({最后状态:h.last_status,结果年龄毫秒:h.last_result_age_ms,连续失败:h.consecutive_failures,历史峰值:h.peak_consecutive_failures,累计失败:h.cumulative_availability==="unavailable"?"当前协议未提供":h.cumulative_failures}));
      const key=operationKey(node,resource),unknown=state.unknownBusOperations.get(key),lease=state.busLeases.get(key),controls=text("div","","bus-reset-controls");
      if(unknown){controls.append(text("p",`复位结果未知（${unknown.operation.operation_id}），禁止重复提交。`,"warning"),text("code",unknown.location || "operation查询链接不可用"));const query=text("button","查询操作状态");query.disabled=!unknown.location;query.addEventListener("click",()=>queryBusOperation(key).catch(e=>alert(e.message)));controls.append(query);}
      else if(state.busResetPermitted){const button=text("button",lease?"确认 Reset":"获取复位租约");button.addEventListener("click",()=> (lease?resetBus(node,resource):acquireBusLease(node,resource)).catch(e=>alert(e.message)));controls.append(button);}
      item.append(controls);
    }
    if((resource.active_alerts || []).length)item.append(renderAlerts(resource.active_alerts)); return item;
  }
  function renderNode(node) {
    const item=document.createElement("details"); item.className="node"; item.open=true;
    const summary=document.createElement("summary"), title=document.createElement("span"); title.append(text("span",node.display_name || node.node_id),text("small",` ${node.board_type || "unknown"}`,"meta")); summary.append(title,chip(node.state)); item.append(summary);
    item.append(kv({node_id:node.node_id,links:node.links,峰值:(node.trend || {}).peaks || {}}),trend((node.trend || {}).samples));
    if((node.active_alerts || []).length)item.append(renderAlerts(node.active_alerts));
    const resources=text("div","","resources"); (node.resources || []).slice(0,MAX_RESOURCES).forEach(r=>{try{resources.append(renderResource(node,r));}catch(_){resources.append(text("p","该资源暂时无法展示。","empty"));}}); item.append(resources); return item;
  }
  function render(data) {
    state.lastOverview=data;
    let remaining=MAX_RESOURCES; const nodes=(data.nodes || []).slice(0,MAX_NODES).map(n=>{const resources=(n.resources || []).slice(0,remaining);remaining-=resources.length;return {...n,resources};}), resources=nodes.flatMap(n=>n.resources);
    $("node-count").textContent=nodes.length; $("available-count").textContent=resources.filter(r=>r.availability==="available").length; $("unavailable-count").textContent=resources.filter(r=>r.availability==="unavailable").length; $("unknown-count").textContent=resources.filter(r=>r.availability==="unknown").length; $("alert-count").textContent=nodes.reduce((n,x)=>n+(x.active_alerts || []).length,0);
    const daemon=$("daemon"), health=data.toolbusd_health || {}; daemon.replaceChildren(text("h2","toolbusd 健康"),chip(health.availability),chip(health.overall),kv({峰值:(health.trend || {}).peaks || {}}),trend((health.trend || {}).samples),renderAlerts(health.threshold_alerts));
    const root=$("nodes"); root.replaceChildren(); nodes.forEach(n=>{try{root.append(renderNode(n));}catch(_){root.append(text("p",`节点 ${n.node_id || "unknown"} 暂时无法展示。`,"empty"));}}); if(!nodes.length)root.append($("empty-template").content.cloneNode(true));
  }
  function known(value) { return value === null || value === undefined ? "unknown" : value; }
  function renderOperations(data) {
    const root=$("operations"), runtime=data.runtime || {}, daemon=data.toolbusd || {}, recording=data.logical_recording || {}, trendStore=data.trend_store || {}, stream=data.overview_stream || {}, errors=data.recent_errors || {};
    const errorList=document.createElement("ul"); errorList.className="alerts";
    (errors.items || []).slice(0,16).forEach(item=>errorList.append(text("li",`${known(item.code)} · ${known(item.age_ms)} ms 前`)));
    root.replaceChildren(text("h2","运维状态"),kv({Runtime版本:known(runtime.version),启动时长毫秒:known(runtime.uptime_ms),toolbusd连接:known(daemon.connection),录制可用性:known(recording.availability),录制已配置:known(recording.configured),录制活动:known(recording.active),录制事件数:known(recording.event_count),录制事件上限:known(recording.maximum_events),趋势持久化:known(trendStore.persistence),趋势序列容量:known(trendStore.sample_capacity_per_series),SSE活动连接:known(stream.active_connections),SSE连接上限:known(stream.maximum_connections)}),text("h3","最近服务错误"),errorList);
    if(!(errors.items || []).length)root.append(text("p","当前没有已记录的服务错误。","empty"));
  }
  async function refreshOperations() {
    const headers={"Accept":"application/json"}, key=$("api-key").value; if(key)headers["X-API-Key"]=key;
    const response=await fetch("/api/v1/operations",{headers,cache:"no-store",credentials:"same-origin"}); const body=await response.json();
    if(!response.ok || !body.ok)throw new Error(body.error?.message || `HTTP ${response.status}`);
    renderOperations(body.data);
  }
  async function refreshCapabilities(){const response=await fetch("/api/v1",{headers:headers(),cache:"no-store",credentials:"same-origin"}),body=await response.json();state.busResetPermitted=Boolean(response.ok&&body.ok&&body.data?.capabilities?.bus_reset?.available&&body.data.capabilities.bus_reset.permitted);}
  async function refresh() {
    if(state.busy)return; state.busy=true; const status=$("connection");
    try { const headers={"Accept":"application/json"}, key=$("api-key").value; if(key)headers["X-API-Key"]=key;
      const response=await fetch("/api/v1/overview",{headers,cache:"no-store",credentials:"same-origin"}); const body=await response.json(); if(!response.ok || !body.ok)throw new Error(body.error?.message || `HTTP ${response.status}`);
      await refreshCapabilities();render(body.data); await refreshOperations(); status.textContent=`已连接 · 快照 ${body.data.snapshot_id} · ${new Date().toLocaleTimeString()}`; status.className="connection ok";
    } catch(error) { status.textContent=`刷新失败：${error instanceof Error ? error.message : "未知错误"}。已保留上次成功数据。`; status.className="connection error"; }
    finally { state.busy=false; }
  }
  function acceptOverview(body) {
    if(!body || !body.ok || !body.data)throw new Error(body?.error?.message || "主动推送数据无效");
    refreshCapabilities().then(()=>render(body.data)).catch(()=>render(body.data)); refreshOperations().catch(()=>{}); const status=$("connection"); status.textContent=`主动推送已连接 · 快照 ${body.data.snapshot_id} · ${new Date().toLocaleTimeString()}`; status.className="connection ok";
  }
  function startPolling(generation) {
    if(generation !== state.generation)return;
    if(state.timer===null)state.timer=setInterval(refresh,REFRESH_MS);
    refresh();
  }
  async function startStream() {
    const generation=++state.generation;
    if(state.stream)state.stream.abort();
    if(state.timer!==null){clearInterval(state.timer);state.timer=null;}
    const controller=new AbortController(); state.stream=controller;
    const headers={"Accept":"text/event-stream"}, key=$("api-key").value; if(key)headers["X-API-Key"]=key;
    try {
      const response=await fetch("/api/v1/overview/stream",{headers,cache:"no-store",credentials:"same-origin",signal:controller.signal});
      if(!response.ok || !response.body)throw new Error(`HTTP ${response.status}`);
      const reader=response.body.getReader(), decoder=new TextDecoder(); let buffer="";
      while(true){
        const result=await reader.read(); if(result.done)throw new Error("连接已断开");
        buffer+=decoder.decode(result.value,{stream:true});
        if(buffer.length>MAX_STREAM_BUFFER)throw new Error("推送事件超过浏览器缓冲上限");
        let boundary;
        while((boundary=buffer.indexOf("\n\n"))>=0){
          const block=buffer.slice(0,boundary);buffer=buffer.slice(boundary+2);
          const data=block.split("\n").filter(line=>line.startsWith("data: ")).map(line=>line.slice(6)).join("\n");
          if(data)acceptOverview(JSON.parse(data));
        }
      }
    } catch(error) {
      if(controller.signal.aborted || generation!==state.generation)return;
      const status=$("connection"); status.textContent=`主动推送中断，已退回轮询：${error instanceof Error ? error.message : "未知错误"}`; status.className="connection error";
      startPolling(generation); setTimeout(()=>{if(generation===state.generation)startStream();},RETRY_STREAM_MS);
    }
  }
  $("api-key").value=""; $("refresh").addEventListener("click",refresh); $("api-key").addEventListener("change",startStream); startStream();
})();
