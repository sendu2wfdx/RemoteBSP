"use strict";
(() => {
  const MAX_NODES = 128, MAX_RESOURCES = 256, REFRESH_MS = 2000;
  const $ = id => document.getElementById(id);
  const state = {busy:false, timer:null};
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
  function renderResource(resource) {
    const item=document.createElement("details"); item.className="resource";
    const summary=document.createElement("summary"); summary.append(text("span",resource.display_name || resource.resource_id || "未命名资源"));
    const chips=text("span","","chips"); chips.append(chip(resource.availability),chip(resource.health)); summary.append(chips); item.append(summary);
    item.append(kv({resource_id:resource.resource_id,kind:resource.kind,...(resource.state || {})}));
    if((resource.active_alerts || []).length)item.append(renderAlerts(resource.active_alerts)); return item;
  }
  function renderNode(node) {
    const item=document.createElement("details"); item.className="node"; item.open=true;
    const summary=document.createElement("summary"), title=document.createElement("span"); title.append(text("span",node.display_name || node.node_id),text("small",` ${node.board_type || "unknown"}`,"meta")); summary.append(title,chip(node.state)); item.append(summary);
    item.append(kv({node_id:node.node_id,links:node.links,峰值:(node.trend || {}).peaks || {}}),trend((node.trend || {}).samples));
    if((node.active_alerts || []).length)item.append(renderAlerts(node.active_alerts));
    const resources=text("div","","resources"); (node.resources || []).slice(0,MAX_RESOURCES).forEach(r=>{try{resources.append(renderResource(r));}catch(_){resources.append(text("p","该资源暂时无法展示。","empty"));}}); item.append(resources); return item;
  }
  function render(data) {
    let remaining=MAX_RESOURCES; const nodes=(data.nodes || []).slice(0,MAX_NODES).map(n=>{const resources=(n.resources || []).slice(0,remaining);remaining-=resources.length;return {...n,resources};}), resources=nodes.flatMap(n=>n.resources);
    $("node-count").textContent=nodes.length; $("available-count").textContent=resources.filter(r=>r.availability==="available").length; $("unavailable-count").textContent=resources.filter(r=>r.availability==="unavailable").length; $("unknown-count").textContent=resources.filter(r=>r.availability==="unknown").length; $("alert-count").textContent=nodes.reduce((n,x)=>n+(x.active_alerts || []).length,0);
    const daemon=$("daemon"), health=data.toolbusd_health || {}; daemon.replaceChildren(text("h2","toolbusd 健康"),chip(health.availability),chip(health.overall),kv({峰值:(health.trend || {}).peaks || {}}),trend((health.trend || {}).samples),renderAlerts(health.threshold_alerts));
    const root=$("nodes"); root.replaceChildren(); nodes.forEach(n=>{try{root.append(renderNode(n));}catch(_){root.append(text("p",`节点 ${n.node_id || "unknown"} 暂时无法展示。`,"empty"));}}); if(!nodes.length)root.append($("empty-template").content.cloneNode(true));
  }
  async function refresh() {
    if(state.busy)return; state.busy=true; const status=$("connection");
    try { const headers={"Accept":"application/json"}, key=$("api-key").value; if(key)headers["X-API-Key"]=key;
      const response=await fetch("/api/v1/overview",{headers,cache:"no-store",credentials:"same-origin"}); const body=await response.json(); if(!response.ok || !body.ok)throw new Error(body.error?.message || `HTTP ${response.status}`);
      render(body.data); status.textContent=`已连接 · 快照 ${body.data.snapshot_id} · ${new Date().toLocaleTimeString()}`; status.className="connection ok";
    } catch(error) { status.textContent=`刷新失败：${error instanceof Error ? error.message : "未知错误"}。已保留上次成功数据。`; status.className="connection error"; }
    finally { state.busy=false; }
  }
  $("api-key").value=""; $("refresh").addEventListener("click",refresh); $("api-key").addEventListener("change",refresh); refresh(); state.timer=setInterval(refresh,REFRESH_MS);
})();
