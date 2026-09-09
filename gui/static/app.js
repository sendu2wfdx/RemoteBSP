const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const roleNames = {step:"STEP",dir:"DIR",enable:"EN",tmc_uart:"TMC UART",limit:"限位/DIAG"};
const pinRoles = ['step','dir','enable','tmc_uart','limit'];
let catalog, board, axes = [], gpios = [], uarts = [], pwms = [], pixelStrips = [], state, demoRunning = false, lastPosition = new Map();
const controlOverrides={gpio:new Map(),pwm:new Map(),strips:new Map()};

function switchView(name){
  $$('.nav-item').forEach(x=>x.classList.toggle('active',x.dataset.view===name));
  $$('.view').forEach(x=>x.classList.toggle('active',x.id===`view-${name}`));
  $('#pageTitle').textContent={twin:'数字孪生',control:'实时控制',config:'资源配置',monitor:'遥测监控'}[name];
}
$$('.nav-item').forEach(x=>x.onclick=()=>switchView(x.dataset.view));

function formatTime(ms){const s=Math.floor(ms/1000);return [Math.floor(s/3600),Math.floor(s/60)%60,s%60].map(x=>String(x).padStart(2,'0')).join(':')}
function formatBytes(bytes){if(bytes<1024)return `${bytes} B`;if(bytes<1024*1024)return `${(bytes/1024).toFixed(1)} KiB`;return `${(bytes/1024/1024).toFixed(1)} MiB`}
function escapeHtml(value){return String(value??'').replace(/[&<>"]/g,char=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[char]))}
function pinPort(pin){return `GPIO${pin[1]}`}
function occupied(exceptAxis=-1,exceptRole='',exceptGpio=-1){
  const used=new Set(Object.keys(board.reserved.reduce((o,x)=>(o[x.pin]=x.owner,o),{})));
  axes.forEach((axis,i)=>pinRoles.forEach(role=>{const pin=axis[role];if(pin&&!(i===exceptAxis&&role===exceptRole))used.add(pin)}));
  gpios.forEach((gpio,index)=>{if(gpio.pin&&index!==exceptGpio)used.add(gpio.pin)});
  uarts.forEach(uart=>{if(uart.rx_pin)used.add(uart.rx_pin);if(uart.tx_pin)used.add(uart.tx_pin);if(uart.direction_pin)used.add(uart.direction_pin)});
  pwms.forEach(pwm=>{if(pwm.pin)used.add(pwm.pin)});
  pixelStrips.forEach(strip=>{if(strip.pin)used.add(strip.pin)});
  return used;
}
function pinOptions(current,index,role,optional=true){
  const used=occupied(index,role), groups={};
  board.pins.forEach(pin=>{if(!used.has(pin)||pin===current)(groups[pinPort(pin)]??=[]).push(pin)});
  let html=optional?'<option value="">未配置</option>':'';
  Object.entries(groups).forEach(([port,pins])=>{html+=`<optgroup label="${port}">${pins.map(pin=>`<option ${pin===current?'selected':''}>${pin}</option>`).join('')}</optgroup>`});
  return html;
}
function syncSharedEnable(){
  axes.forEach((axis,index)=>{const source=axis.enable_source;if(Number.isInteger(source)&&source>=0&&source<index){axis.enable=axes[source].enable;axis.enable_active_low=axes[source].enable_active_low}else if(source!==null){axis.enable_source=null}});
}
function gpioInterface(pin){return (board.gpio_interfaces||[]).find(item=>item.pin===pin)}
function gpioPinOptions(current,index){
  const used=occupied(-1,'',index),groups={};
  board.pins.forEach(pin=>{if(!used.has(pin)||pin===current)(groups[pinPort(pin)]??=[]).push(pin)});
  let html='<option value="">未配置</option>';
  Object.entries(groups).forEach(([port,pins])=>{html+=`<optgroup label="${port}">${pins.map(pin=>{const known=gpioInterface(pin);return `<option value="${pin}" ${pin===current?'selected':''}>${pin}${known?` · ${known.label}`:''}</option>`}).join('')}</optgroup>`});
  return html;
}
function applyGpioConstraint(gpio){
  const known=gpioInterface(gpio.pin);if(!known)return;
  if(!known.allowed_directions.includes(gpio.direction))gpio.direction=known.default_direction;
  if(!known.allowed_pulls.includes(gpio.pull))gpio.pull=known.default_pull;
  gpio.active_low=known.active_low;gpio.safe_level=known.safe_level;gpio.debounce_ms=known.debounce_ms;
  if(!gpio.name||gpio.name.startsWith('gpio_'))gpio.name=known.id;
}
function gpioDirectionOptions(gpio,known){const allowed=known?.allowed_directions||['input','output'];return [['input','输入'],['output','输出']].filter(x=>allowed.includes(x[0])).map(x=>`<option value="${x[0]}" ${gpio.direction===x[0]?'selected':''}>${x[1]}</option>`).join('')}
function gpioPullOptions(gpio,known){const allowed=gpio.direction==='output'?['none']:(known?.allowed_pulls||['none','up','down']);return [['none','浮空/无'],['up','上拉'],['down','下拉']].filter(x=>allowed.includes(x[0])).map(x=>`<option value="${x[0]}" ${gpio.pull===x[0]?'selected':''}>${x[1]}</option>`).join('')}
function renderGpioTable(){
  $('#gpioTable').innerHTML=gpios.map((gpio,index)=>{applyGpioConstraint(gpio);const known=gpioInterface(gpio.pin),input=gpio.direction==='input';return `<div class="resource-row"><div class="axis-row-head"><b>${escapeHtml(gpio.name||`GPIO ${index+1}`)}</b><button data-remove-gpio="${index}">移除</button></div><div class="resource-fields"><label>逻辑名称<input data-gpio="${index}" data-gpio-role="name" value="${escapeHtml(gpio.name||'')}" maxlength="32" placeholder="例如 door_sensor"></label><label>接口/引脚<select data-gpio="${index}" data-gpio-role="pin">${gpioPinOptions(gpio.pin,index)}</select></label><label>方向<select data-gpio="${index}" data-gpio-role="direction" ${known&&known.allowed_directions.length===1?'disabled':''}>${gpioDirectionOptions(gpio,known)}</select></label><label>内部上下拉<select data-gpio="${index}" data-gpio-role="pull" ${known&&known.allowed_pulls.length===1?'disabled':''}>${gpioPullOptions(gpio,known)}</select></label><label>有效电平<select data-gpio="${index}" data-gpio-role="active_low" ${known?'disabled':''}><option value="high" ${!gpio.active_low?'selected':''}>高有效</option><option value="low" ${gpio.active_low?'selected':''}>低有效</option></select></label><label>故障安全电平<select data-gpio="${index}" data-gpio-role="safe_level" ${input?'disabled':''}><option value="low" ${gpio.safe_level!==true?'selected':''}>低电平</option><option value="high" ${gpio.safe_level===true?'selected':''}>高电平</option></select></label><label>输入消抖<input data-gpio="${index}" data-gpio-role="debounce_ms" type="number" min="0" max="1000" value="${gpio.debounce_ms||0}" ${input?'':'disabled'}><span>ms</span></label><div class="constraint-note">${known?`板级接口：${known.label}，方向与电气属性由原理图约束。`:'通用引脚：请按实际外部电路确认方向和安全电平。'}</div></div></div>`}).join('')||'<p class="muted">尚未创建数字 IO 资源。</p>';
  $$('[data-gpio]').forEach(control=>control.onchange=()=>{const gpio=gpios[+control.dataset.gpio],role=control.dataset.gpioRole;if(role==='active_low')gpio[role]=control.value==='low';else if(role==='safe_level')gpio[role]=control.value==='high';else if(role==='debounce_ms')gpio[role]=Math.max(0,Math.min(1000,+control.value||0));else gpio[role]=control.value||null;if(role==='direction'){if(gpio.direction==='output'){gpio.pull='none';if(gpio.safe_level===null)gpio.safe_level=false}else gpio.safe_level=null}if(role==='pin')applyGpioConstraint(gpio);renderResources()});
  $$('[data-remove-gpio]').forEach(button=>button.onclick=()=>{gpios.splice(+button.dataset.removeGpio,1);renderResources()});
}
function capablePinOptions(pins,current){const used=occupied();if(current)used.delete(current);const available=(pins||[]).filter(pin=>!used.has(pin)||pin===current);return '<option value="">未映射</option>'+available.map(pin=>`<option value="${pin}" ${pin===current?'selected':''}>${pin}</option>`).join('')}
function uartTemplates(){return board.uart?.endpoints||[]}
function uartEndpointOptions(current,port){
  return uartTemplates().filter(endpoint=>endpoint.port===port).map(endpoint=>`<option value="${endpoint.endpoint_id}" ${endpoint.endpoint_id===current?'selected':''}>USART${endpoint.port+1} · RX ${endpoint.rx_pin} / TX ${endpoint.tx_pin}</option>`).join('');
}
function applyUartEndpoint(item,endpointId){
  const endpoint=uartTemplates().find(x=>x.endpoint_id===endpointId);if(!endpoint)return;
  const preserved={name:item.name,baud_rate:item.baud_rate};Object.assign(item,structuredClone(endpoint),preserved,{enabled:true,direction_pin:null});
}
function renderUartConfig(){
  $('#uartTable').innerHTML=uarts.map((uart,index)=>`<div class="resource-row"><div class="axis-row-head"><b>${escapeHtml(uart.name||`uart_${index}`)}</b><span class="endpoint-status implemented">USART${uart.port+1} · 固件已实现</span><button data-remove-uart="${index}">移除</button></div><div class="resource-fields"><label>逻辑名称<input data-uart="${index}" data-uart-role="name" value="${escapeHtml(uart.name||'')}"></label><label class="endpoint-field">硬件端点<select data-uart="${index}" data-uart-role="endpoint_id">${uartEndpointOptions(uart.endpoint_id,uart.port)}</select></label><label>RX 引脚<input value="${uart.rx_pin}" disabled></label><label>TX 引脚<input value="${uart.tx_pin}" disabled></label><label>固定波特率<input type="number" min="${uart.minimum_baud_rate}" max="${uart.maximum_baud_rate}" data-uart="${index}" data-uart-role="baud_rate" value="${uart.baud_rate}"><span>bit/s</span></label><div class="constraint-note">普通USART与TMC单线UART相互独立；当前实体后端暂不支持RS-485 DE/RE方向引脚。F103/G431按UART 0→1→2连续裁剪，不能跳过中间端口。</div></div></div>`).join('')||'<p class="muted">此板卡尚未启用普通硬件 UART 资源。</p>';
  $$('[data-uart]').forEach(control=>control.onchange=()=>{const item=uarts[+control.dataset.uart],role=control.dataset.uartRole;if(role==='endpoint_id')applyUartEndpoint(item,control.value);else if(role==='baud_rate')item.baud_rate=Math.max(item.minimum_baud_rate,Math.min(item.maximum_baud_rate,+control.value||item.minimum_baud_rate));else item[role]=control.value||null;renderResources()});
  $$('[data-remove-uart]').forEach(button=>button.onclick=()=>{const port=uarts[+button.dataset.removeUart].port;uarts=uarts.filter(item=>item.port<port);renderResources()});
}
function waveformTemplates(kind){return board.waveform?.[kind]||[]}
function endpointOptions(kind,current,items){
  const used=new Set(items.map(item=>item.endpoint_id).filter(id=>id&&id!==current));
  return waveformTemplates(kind).map(endpoint=>`<option value="${endpoint.endpoint_id}" ${endpoint.endpoint_id===current?'selected':''} ${used.has(endpoint.endpoint_id)?'disabled':''}>${endpoint.timer||endpoint.timer_dma} · ${endpoint.pin} · ${endpoint.backend_status==='implemented'?'已实现':'待验证'}</option>`).join('');
}
function applyEndpoint(item,kind,endpointId){
  const templates=waveformTemplates(kind),base=templates[0]||{},endpoint=templates.find(x=>x.endpoint_id===endpointId);if(!endpoint)return;
  const preserved=kind==='pwm'?{name:item.name,frequency_hz:item.frequency_hz,default_duty_percent:item.default_duty_percent,active_low:item.active_low}:{name:item.name,pixel_count:item.pixel_count,color_order:item.color_order,reset_time_us:item.reset_time_us};
  Object.assign(item,structuredClone(base),structuredClone(endpoint),preserved,{enabled:true,pin:endpoint.pin});
}
function renderPwmConfig(){
  $('#pwmTable').innerHTML=pwms.map((pwm,index)=>`<div class="resource-row waveform-row"><div class="axis-row-head"><b>${escapeHtml(pwm.name||`pwm_${index}`)}</b><span class="endpoint-status ${pwm.backend_status}">${pwm.backend_status==='implemented'?'固件已实现':'后端待验证'}</span><button data-remove-pwm="${index}">移除</button></div><div class="resource-fields"><label>逻辑名称<input data-pwm="${index}" data-wave-role="name" value="${escapeHtml(pwm.name||'')}"></label><label class="endpoint-field">硬件端点<select data-pwm="${index}" data-wave-role="endpoint_id">${endpointOptions('pwm',pwm.endpoint_id,pwms)}</select></label><label>输出引脚<select data-pwm="${index}" data-wave-role="pin">${capablePinOptions(pwm.capable_pins,pwm.pin)}</select></label><label>固定频率<input type="number" min="1" max="100000" data-pwm="${index}" data-wave-role="frequency_hz" value="${pwm.frequency_hz}"><span>Hz</span></label><label>默认占空比<input type="number" min="0" max="100" step="0.1" data-pwm="${index}" data-wave-role="default_duty_percent" value="${pwm.default_duty_percent}"><span>%</span></label><label>有效极性<select data-pwm="${index}" data-wave-role="active_low"><option value="high" ${!pwm.active_low?'selected':''}>高有效</option><option value="low" ${pwm.active_low?'selected':''}>低有效</option></select></label><div class="constraint-note">通道 ${pwm.channel} · ${pwm.timer} · ${pwm.frequency_group}共享频率；实时占空比在“实时控制”页面修改。</div></div></div>`).join('')||'<p class="muted">尚未添加 PWM 资源。点击“添加 PWM”创建。</p>';
  $$('[data-pwm]').forEach(control=>control.onchange=()=>{const item=pwms[+control.dataset.pwm],role=control.dataset.waveRole;if(role==='endpoint_id')applyEndpoint(item,'pwm',control.value);else if(role==='active_low')item.active_low=control.value==='low';else if(role==='frequency_hz')item.frequency_hz=Math.max(1,+control.value||1);else if(role==='default_duty_percent')item.default_duty_percent=Math.max(0,Math.min(100,+control.value||0));else item[role]=control.value||null;renderResources()});
  $$('[data-remove-pwm]').forEach(button=>button.onclick=()=>{pwms.splice(+button.dataset.removePwm,1);renderResources()});
}
function renderStripConfig(){
  $('#stripTable').innerHTML=pixelStrips.map((strip,index)=>`<div class="resource-row waveform-row"><div class="axis-row-head"><b>${escapeHtml(strip.name||`strip_${index}`)}</b><span class="endpoint-status ${strip.backend_status}">${strip.backend_status==='implemented'?'固件已实现':'后端待验证'}</span><button data-remove-strip="${index}">移除</button></div><div class="resource-fields"><label>逻辑名称<input data-strip="${index}" data-wave-role="name" value="${escapeHtml(strip.name||'')}"></label><label class="endpoint-field">硬件端点<select data-strip="${index}" data-wave-role="endpoint_id">${endpointOptions('ws2812',strip.endpoint_id,pixelStrips)}</select></label><label>输出引脚<select data-strip="${index}" data-wave-role="pin">${capablePinOptions(strip.capable_pins,strip.pin)}</select></label><label>像素数量<input type="number" min="1" max="${strip.max_pixels}" data-strip="${index}" data-wave-role="pixel_count" value="${strip.pixel_count}"></label><label>色序<select data-strip="${index}" data-wave-role="color_order"><option ${strip.color_order==='GRB'?'selected':''}>GRB</option><option ${strip.color_order==='RGB'?'selected':''}>RGB</option><option ${strip.color_order==='BRG'?'selected':''}>BRG</option></select></label><label>复位时间<input type="number" min="50" max="1000" data-strip="${index}" data-wave-role="reset_time_us" value="${strip.reset_time_us}"><span>µs</span></label><div class="constraint-note">通道 ${strip.channel} · ${strip.timer_dma} · 800 kbit/s；颜色和亮度是运行时数据。</div></div></div>`).join('')||'<p class="muted">尚未添加 WS2812 灯带。点击“添加灯带”创建。</p>';
  $$('[data-strip]').forEach(control=>control.onchange=()=>{const item=pixelStrips[+control.dataset.strip],role=control.dataset.waveRole;if(role==='endpoint_id')applyEndpoint(item,'ws2812',control.value);else if(role==='pixel_count')item.pixel_count=Math.max(1,Math.min(item.max_pixels,+control.value||1));else if(role==='reset_time_us')item.reset_time_us=Math.max(50,Math.min(1000,+control.value||80));else item[role]=control.value||null;renderResources()});
  $$('[data-remove-strip]').forEach(button=>button.onclick=()=>{pixelStrips.splice(+button.dataset.removeStrip,1);renderResources()});
}
function renderResources(){renderUartConfig();renderPwmConfig();renderStripConfig();renderAxisTable();renderGpioTable();validate();const availablePorts=new Set(uartTemplates().map(x=>x.port));$('#addUart').disabled=uarts.length>=availablePorts.size;$('#addPwm').disabled=pwms.length>=waveformTemplates('pwm').length;$('#addStrip').disabled=pixelStrips.length>=waveformTemplates('ws2812').length}
function enableSourceOptions(index,source){return `<option value="own" ${source===null?'selected':''}>独立 EN</option>`+axes.slice(0,index).map((axis,i)=>`<option value="${i}" ${source===i?'selected':''}>与轴 ${i+1} 共用 · ${axis.enable||'未配置'}</option>`).join('')}
function normalizeAxisDriver(axis){
  if(!axis.driver_type)axis.driver_type=axis.tmc_uart?'tmc2209_uart':'none';
  if(!Number.isInteger(axis.tmc_address))axis.tmc_address=0;
  if(axis.driver_type==='none')axis.tmc_uart=null;
  return axis;
}
function renderAxisTable(){
  syncSharedEnable();
  $('#axisTable').innerHTML=axes.map((axis,index)=>{normalizeAxisDriver(axis);const shared=Number.isInteger(axis.enable_source),tmc=axis.driver_type==='tmc2209_uart';return `<div class="axis-row"><div class="axis-row-head"><b>轴 ${index+1}</b><button data-remove="${index}">移除</button></div><div class="pin-fields"><label>STEP<select data-axis="${index}" data-role="step">${pinOptions(axis.step,index,'step',false)}</select></label><label>DIR<select data-axis="${index}" data-role="dir">${pinOptions(axis.dir,index,'dir',false)}</select></label><label>DIR 方向<select data-dir-inverted="${index}"><option value="normal" ${!axis.dir_inverted?'selected':''}>正常</option><option value="inverted" ${axis.dir_inverted?'selected':''}>反相</option></select></label><label>EN 来源<select data-enable-source="${index}">${enableSourceOptions(index,axis.enable_source)}</select></label><label>EN<select data-enable-pin="${index}" ${shared?'disabled':''}>${shared?`<option>${axis.enable}</option>`:pinOptions(axis.enable,index,'enable',false)}</select></label><label>EN 极性<select data-enable-polarity="${index}" ${shared?'disabled':''}><option value="low" ${axis.enable_active_low?'selected':''}>低有效</option><option value="high" ${!axis.enable_active_low?'selected':''}>高有效</option></select></label><label>驱动器<select data-driver-type="${index}"><option value="none" ${!tmc?'selected':''}>仅 STEP/DIR</option><option value="tmc2209_uart" ${tmc?'selected':''}>TMC2209 单线 UART</option></select></label><label>TMC UART<select data-axis="${index}" data-role="tmc_uart" ${tmc?'':'disabled'}>${pinOptions(axis.tmc_uart,index,'tmc_uart',true)}</select></label><label>TMC 地址<select data-tmc-address="${index}" ${tmc?'':'disabled'}>${[0,1,2,3].map(value=>`<option value="${value}" ${axis.tmc_address===value?'selected':''}>${value}</option>`).join('')}</select></label><label>限位/DIAG<select data-axis="${index}" data-role="limit">${pinOptions(axis.limit,index,'limit',true)}</select></label></div></div>`}).join('')||'<p class="muted">尚未添加运动轴。</p>';
  $$('[data-axis]').forEach(select=>select.onchange=()=>{axes[+select.dataset.axis][select.dataset.role]=select.value||null;renderResources()});
  $$('[data-driver-type]').forEach(select=>select.onchange=()=>{const axis=axes[+select.dataset.driverType];axis.driver_type=select.value;if(select.value==='none')axis.tmc_uart=null;renderResources()});
  $$('[data-tmc-address]').forEach(select=>select.onchange=()=>{axes[+select.dataset.tmcAddress].tmc_address=+select.value});
  $$('[data-enable-source]').forEach(select=>select.onchange=()=>{axes[+select.dataset.enableSource].enable_source=select.value==='own'?null:+select.value;renderResources()});
  $$('[data-dir-inverted]').forEach(select=>select.onchange=()=>{axes[+select.dataset.dirInverted].dir_inverted=select.value==='inverted'});
  $$('[data-enable-pin]').forEach(select=>select.onchange=()=>{axes[+select.dataset.enablePin].enable=select.value;renderResources()});
  $$('[data-enable-polarity]').forEach(select=>select.onchange=()=>{axes[+select.dataset.enablePolarity].enable_active_low=select.value==='low';renderAxisTable();validate()});
  $$('[data-remove]').forEach(button=>button.onclick=()=>{const removed=+button.dataset.remove;axes.splice(removed,1);axes.forEach(axis=>{if(axis.enable_source===removed)axis.enable_source=null;else if(axis.enable_source>removed)axis.enable_source--});renderResources()});
}
function validate(){
  const errors=[]; axes.forEach((axis,i)=>{['step','dir','enable'].forEach(role=>{if(!axis[role])errors.push(`轴 ${i+1} 缺少 ${roleNames[role]}`)});if(axis.driver_type==='tmc2209_uart'&&!axis.tmc_uart)errors.push(`轴 ${i+1} 缺少 TMC UART`)});
  const names=new Set();gpios.forEach((gpio,index)=>{if(!gpio.name)errors.push(`数字 IO ${index+1} 缺少逻辑名称`);else if(names.has(gpio.name))errors.push(`数字 IO 逻辑名称重复：${gpio.name}`);else names.add(gpio.name);if(!gpio.pin)errors.push(`数字 IO ${index+1} 缺少引脚`)});
  const uartPorts=new Set();uarts.forEach((uart,index)=>{if(!uart.name)errors.push(`UART ${index+1} 缺少逻辑名称`);else if(names.has(uart.name))errors.push(`资源逻辑名称重复：${uart.name}`);else names.add(uart.name);if(uartPorts.has(uart.port))errors.push(`UART 硬件端口 ${uart.port} 重复`);else uartPorts.add(uart.port);if(!uart.rx_pin||!uart.tx_pin)errors.push(`UART ${index+1} 缺少RX/TX端点`);if(uart.baud_rate<uart.minimum_baud_rate||uart.baud_rate>uart.maximum_baud_rate)errors.push(`UART ${uart.name} 波特率超出硬件范围`);if(uart.backend_status!=='implemented')errors.push(`UART ${uart.name} 实体后端尚未实现`)});
  const pwmChannels=new Set(),pwmEndpoints=new Set(),frequencyGroups=new Map();pwms.forEach((pwm,index)=>{if(!pwm.name)errors.push(`PWM ${index+1} 缺少逻辑名称`);else if(names.has(pwm.name))errors.push(`资源逻辑名称重复：${pwm.name}`);else names.add(pwm.name);if(!pwm.pin)errors.push(`PWM ${index+1} 尚未映射到可用引脚`);if(pwmChannels.has(pwm.channel))errors.push(`PWM 硬件通道 ${pwm.channel} 重复`);else pwmChannels.add(pwm.channel);if(pwmEndpoints.has(pwm.endpoint_id))errors.push(`PWM 端点 ${pwm.endpoint_id} 重复`);else pwmEndpoints.add(pwm.endpoint_id);if(pwm.backend_status!=='implemented')errors.push(`PWM ${pwm.name} 使用的 ${pwm.timer} 固件后端尚未验证`);const known=frequencyGroups.get(pwm.frequency_group);if(known!==undefined&&known!==pwm.frequency_hz)errors.push(`${pwm.frequency_group} 的多个PWM通道必须使用相同频率`);else frequencyGroups.set(pwm.frequency_group,pwm.frequency_hz)});
  const stripChannels=new Set(),stripEndpoints=new Set(),dmaResources=new Set();pixelStrips.forEach((strip,index)=>{if(!strip.name)errors.push(`灯带 ${index+1} 缺少逻辑名称`);else if(names.has(strip.name))errors.push(`资源逻辑名称重复：${strip.name}`);else names.add(strip.name);if(!strip.pin)errors.push(`灯带 ${index+1} 尚未映射到可用引脚`);if(stripChannels.has(strip.channel))errors.push(`定时位流通道 ${strip.channel} 重复`);else stripChannels.add(strip.channel);if(stripEndpoints.has(strip.endpoint_id))errors.push(`定时位流端点 ${strip.endpoint_id} 重复`);else stripEndpoints.add(strip.endpoint_id);if(dmaResources.has(strip.dma_resource))errors.push(`DMA资源 ${strip.dma_resource} 重复`);else dmaResources.add(strip.dma_resource);if(strip.backend_status!=='implemented')errors.push(`灯带 ${strip.name} 使用的 ${strip.timer_dma} 固件后端尚未验证`)});
  const selections=[];axes.forEach(axis=>pinRoles.forEach(role=>axis[role]&&selections.push([axis[role],`运动 ${roleNames[role]}`])));gpios.forEach(gpio=>gpio.pin&&selections.push([gpio.pin,`GPIO ${gpio.name}`]));uarts.forEach(uart=>{uart.rx_pin&&selections.push([uart.rx_pin,`UART ${uart.name} RX`]);uart.tx_pin&&selections.push([uart.tx_pin,`UART ${uart.name} TX`]);uart.direction_pin&&selections.push([uart.direction_pin,`UART ${uart.name}方向控制`])});pwms.forEach(x=>x.pin&&selections.push([x.pin,`PWM ${x.name}`]));pixelStrips.forEach(x=>x.pin&&selections.push([x.pin,`WS2812 ${x.name}`]));const owners=new Map();selections.forEach(([pin,owner])=>{if(owners.has(pin))errors.push(`${pin} 同时用于 ${owners.get(pin)} 与 ${owner}`);else owners.set(pin,owner)});
  $('#validation').className=errors.length?'invalid':'valid';
  const shared=axes.filter(axis=>Number.isInteger(axis.enable_source)).length;
  $('#validation').innerHTML=errors.length?errors.map(x=>`<div>• ${x}</div>`).join(''):`✓ ${axes.length} 个轴、${gpios.length} 个数字 IO、${uarts.length} 路普通 UART、${pwms.length} 路 PWM、${pixelStrips.length} 条灯带，引脚无冲突${shared?`；${shared} 个轴复用 EN`:''}`;
}
function loadBoard(id){
  board=catalog.boards.find(x=>x.id===id);axes=structuredClone(board.motion_defaults).map(normalizeAxisDriver);gpios=structuredClone(board.gpio_defaults||[]);
  uarts=structuredClone(board.uart_defaults||[]);
  pwms=structuredClone(board.waveform?.pwm||[]).filter(x=>x.enabled).map((x,index)=>({...x,name:x.name||`pwm_${index}`,channel:x.channel??index,default_duty_percent:x.default_duty_percent??x.duty_percent??50}));
  pixelStrips=structuredClone(board.waveform?.ws2812||[]).filter(x=>x.enabled).map((x,index)=>({...x,name:x.name||`strip_${index}`,channel:x.channel??index,color_order:x.color_order||'GRB',reset_time_us:x.reset_time_us||80}));
  $('#reservedPins').innerHTML=board.reserved.map(x=>`<span class="chip" title="${x.owner}">${x.pin} · ${x.owner}</span>`).join('');
  renderResources();
}
function currentManifest(){return {schema_version:2,board_id:board.id,generated_by:'RemoteBSP Studio',gpio:{resources:gpios},uart:{ports:uarts.map(x=>({name:x.name,endpoint_id:x.endpoint_id,port:x.port,baud_rate:x.baud_rate,direction_pin:x.direction_pin}))},pwm:{channels:pwms.map(x=>({name:x.name,endpoint_id:x.endpoint_id,channel:x.channel,pin:x.pin,frequency_hz:x.frequency_hz,default_duty_percent:x.default_duty_percent,active_low:x.active_low}))},timed_bitstream:{ws2812:pixelStrips.map(x=>({name:x.name,endpoint_id:x.endpoint_id,channel:x.channel,pin:x.pin,pixel_count:x.pixel_count,color_order:x.color_order,reset_time_us:x.reset_time_us}))},motion:{axes},i2c:{buses:[],devices:[]},spi:{buses:[],devices:[]}}}
function exportManifest(){
  const manifest=currentManifest();
  const blob=new Blob([JSON.stringify(manifest,null,2)],{type:'application/json'}),link=document.createElement('a');
  link.href=URL.createObjectURL(blob);link.download=`${board.id}-resources.json`;link.click();URL.revokeObjectURL(link.href);
}
async function compileManifest(){
  const result=$('#compileResult');result.textContent='正在校验资源并生成固件配置…';result.className='compile-result';
  try{const response=await fetch('/api/project/generate',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({project:currentManifest()})}),value=await response.json();if(!response.ok||!value.ok)throw new Error(value.error||'生成失败');const bytes=Uint8Array.from(atob(value.config_base64),x=>x.charCodeAt(0)),blob=new Blob([bytes],{type:'text/plain;charset=utf-8'}),link=document.createElement('a');link.href=URL.createObjectURL(blob);link.download=value.filename;link.click();URL.revokeObjectURL(link.href);result.textContent=`✓ 已生成 ${value.firmware_target} 固件配置：${value.resource_count} 项资源，${value.byte_count} 字节；复制到 firmware/.config 后构建`;result.className='compile-result valid'}catch(error){result.textContent=`生成失败：${error.message}`;result.className='compile-result invalid'}
}
async function buildFirmware(){
  const result=$('#compileResult'),button=$('#buildFirmware'),configButton=$('#compileConfig');button.disabled=true;configButton.disabled=true;result.textContent='正在校验并构建固件，请稍候…';result.className='compile-result';
  try{const response=await fetch('/api/project/build',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({project:currentManifest()})}),value=await response.json();if(!response.ok||!value.ok)throw new Error(value.error||'构建失败');const links=value.artifacts.map(item=>`<a href="${escapeHtml(item.url)}" download>${escapeHtml(item.filename)} · ${formatBytes(item.size)}</a>`).join(''),ram=value.memory?.ram,flash=value.memory?.flash,memory=ram&&flash?`RAM ${formatBytes(ram.used_bytes)} / ${formatBytes(ram.capacity_bytes)} · ${ram.used_percent.toFixed(2)}%<br>Flash ${formatBytes(flash.used_bytes)} / ${formatBytes(flash.capacity_bytes)} · ${flash.used_percent.toFixed(2)}%<br>`:'';result.innerHTML=`<b>✓ ${escapeHtml(value.firmware_target)} 构建完成</b><small>${memory}构建ID ${escapeHtml(value.build_id)}<br>配置 SHA-256 ${escapeHtml(value.config_sha256)}</small><div class="artifact-links">${links}</div>`;result.className='compile-result valid'}catch(error){result.textContent=`构建失败：${error.message}`;result.className='compile-result invalid'}finally{button.disabled=false;configButton.disabled=false}
}

function renderGpio(items){$('#gpioGrid').innerHTML=items.map(x=>`<div class="gpio"><div><label>GPIO ${x.pin}</label><small>${x.direction.toUpperCase()}</small></div><i class="level ${x.value?'high':''}"></i></div>`).join('')||'<p class="muted">暂无已配置 GPIO</p>'}
function motorAngle(axis){const steps=Math.max(1,axis.steps_per_revolution||200),position=((axis.position_steps%steps)+steps)%steps;return Math.round(position/steps*3600)/10}
function renderAxes(items){
  $('#axisGrid').innerHTML=items.map((x,i)=>{lastPosition.set(x.resource_id,x.position_steps);const angle=motorAngle(x);return `<div class="axis"><div class="motor" style="--motor-angle:${angle}deg" role="img" aria-label="轴 ${i+1} 当前位置 ${angle.toFixed(1)} 度"><i class="motor-corner c1"></i><i class="motor-corner c2"></i><i class="motor-corner c3"></i><i class="motor-corner c4"></i><div class="motor-face"><div class="motor-pointer"><i></i></div><div class="motor-shaft"></div></div><div class="motor-plug"></div></div><div class="axis-info"><b>AXIS ${i+1}</b><small>${x.position_steps.toLocaleString()} steps</small><small>位置指针 ${angle.toFixed(1)}°</small><small class="axis-state ${x.enabled?'enabled':''}">${x.enabled?'ENABLED':'SAFE / DISABLED'}</small></div></div>`}).join('')||'<p class="muted">此 Mock 板卡没有运动轴</p>';
}
function renderPixels(ws){
  const pixels=ws?.strips?.[0]?.pixels||Array(8).fill('#111827');
  $('#pixelStrip').innerHTML=pixels.map(color=>`<i class="pixel-dot" style="--pixel:${color}"></i>`).join('');
  $('#wsHint').textContent=ws?.supported?'Mock 已按定时位流解码颜色；STM32 后端使用 TIM1 PWM + DMA，尚待示波器与实灯带验收。':'当前 Mock 未提供定时位流资源；页面保留灰色占位。';
}
function renderPwm(items){$('#pwmGrid').innerHTML=(items||[]).map(x=>`<div class="pwm-state"><div class="pwm-dial" style="--duty:${Math.round(x.duty/100)}"><i></i></div><div><b>PWM ${x.channel}</b><small>${x.frequency_hz.toLocaleString()} Hz</small><strong>${(x.duty/100).toFixed(1)}%</strong><small>${x.running?'RUNNING':'STOPPED'}</small></div></div>`).join('')||'<p class="muted">暂无运行中的 PWM 对象</p>'}
function applyControlOverrides(value){
  value.gpio.forEach(item=>{if(controlOverrides.gpio.has(item.pin))item.value=controlOverrides.gpio.get(item.pin)});
  value.pwm.forEach(item=>{const key=item.resource_id??item.channel,override=controlOverrides.pwm.get(key);if(override){item.duty=override.duty;item.running=override.running}});
  (value.ws2812?.strips||[]).forEach((strip,index)=>{const key=strip.resource_id??index,override=controlOverrides.strips.get(key);if(override)strip.pixels=strip.pixels.map(()=>override.color)});
}
function renderLiveControls(value){
  const outputs=value.gpio.filter(item=>item.direction==='output');
  $('#controlGpio').innerHTML=outputs.map(item=>`<div class="live-control-row"><div><b>GPIO ${item.pin}</b><small>数字输出</small></div><label class="live-switch"><input type="checkbox" data-live-gpio="${item.pin}" ${item.value?'checked':''}><span></span>${item.value?'HIGH':'LOW'}</label></div>`).join('')||'<p class="muted">当前节点没有 GPO 对象。</p>';
  $('#controlPwm').innerHTML=(value.pwm||[]).map(item=>{const key=item.resource_id??item.channel;return `<div class="live-control-row live-pwm"><div><b>PWM ${item.channel}</b><small>${item.frequency_hz.toLocaleString()} Hz · 固定频率</small></div><label>实时占空比<input type="range" min="0" max="10000" step="10" value="${item.duty}" data-live-pwm-duty="${key}"><output>${(item.duty/100).toFixed(1)}%</output></label><button class="${item.running?'stop':'start'}" data-live-pwm-toggle="${key}">${item.running?'停止':'启动'}</button></div>`}).join('')||'<p class="muted">当前节点没有 PWM 对象。</p>';
  const strips=value.ws2812?.strips||[];
  $('#controlStrips').innerHTML=strips.map((strip,index)=>{const key=strip.resource_id??index,color=strip.pixels?.[0]||'#43d9bd';return `<div class="live-control-row live-strip"><div><b>灯带 ${index}</b><small>${strip.pixels.length} 像素</small></div><label>全部颜色<input type="color" value="${color}" data-live-strip-color="${key}"></label><div class="live-pixels">${strip.pixels.slice(0,24).map(pixel=>`<i style="--pixel:${pixel}"></i>`).join('')}</div></div>`}).join('')||'<p class="muted">当前节点没有灯带对象。</p>';
  $$('[data-live-gpio]').forEach(control=>control.onchange=()=>{controlOverrides.gpio.set(+control.dataset.liveGpio,control.checked);applyControlOverrides(state);renderState(state)});
  $$('[data-live-pwm-duty]').forEach(control=>control.oninput=()=>{const key=+control.dataset.livePwmDuty,item=(state.pwm||[]).find(x=>(x.resource_id??x.channel)===key);controlOverrides.pwm.set(key,{duty:+control.value,running:item?.running??true});applyControlOverrides(state);renderState(state)});
  $$('[data-live-pwm-toggle]').forEach(control=>control.onclick=()=>{const key=+control.dataset.livePwmToggle,item=(state.pwm||[]).find(x=>(x.resource_id??x.channel)===key),old=controlOverrides.pwm.get(key);controlOverrides.pwm.set(key,{duty:old?.duty??item?.duty??0,running:!(old?.running??item?.running)});applyControlOverrides(state);renderState(state)});
  $$('[data-live-strip-color]').forEach(control=>control.oninput=()=>{controlOverrides.strips.set(+control.dataset.liveStripColor,{color:control.value});applyControlOverrides(state);renderState(state)});
}
function renderState(value){
  const demo=value.source==='demo';
  state=value;applyControlOverrides(value);$('#boardName').textContent=value.board_name;$('#nodeState').textContent=demo?'演示':(value.online?'在线':'离线');
  $('#sideDot').classList.toggle('online',!demo&&value.online);$('#sideStatus').textContent=demo?'演示模式':(value.online?'节点在线':'节点离线');$('#sideSource').textContent=value.source==='mock_mcu'?'Mock MCU 实时数据':'内置演示数据';
  $('#elapsed').textContent=formatTime(value.elapsed_ms);$('#motionState').textContent=value.motion.state.toUpperCase();$('#motionFault').textContent=value.motion.fault.toUpperCase();$('#queueDepth').textContent=`${value.motion.queue_depth} / ${value.motion.queue_capacity}`;
  renderGpio(value.gpio);renderAxes(value.motion.axes);renderPwm(value.pwm);renderPixels(value.ws2812);renderLiveControls(value);
  $('#monOnline').textContent=demo?'DEMO':(value.online?'ONLINE':'OFFLINE');$('#monElapsed').textContent=`${value.elapsed_ms.toLocaleString()} ms`;$('#monGpio').textContent=value.gpio.length;$('#monSteps').textContent=value.motion.axes.reduce((n,x)=>n+x.emitted_steps,0).toLocaleString();
  $('#timeline').innerHTML=[['状态快照已更新',`${formatTime(value.elapsed_ms)} · ${value.source}`],[`运动内核 ${value.motion.state.toUpperCase()}`,`queue ${value.motion.queue_depth}/${value.motion.queue_capacity}`],[value.ws2812?.supported?'WS2812 预览可用':'WS2812 后端未启用','resource capability']].map(x=>`<div class="event"><b>${x[0]}</b><small>${x[1]}</small></div>`).join('');
}
async function refresh(){try{const response=await fetch('/api/state',{cache:'no-store'});renderState(await response.json())}catch(error){$('#sideStatus').textContent='GUI 服务断开';$('#sideDot').classList.remove('online')}}
function animateDemo(){if(!demoRunning||state?.source!=='demo')return;state.elapsed_ms+=100;state.motion.state='running';state.motion.axes.forEach((x,i)=>{x.enabled=true;x.position_steps+=(i+1)*7;x.emitted_steps+=Math.abs((i+1)*7)});renderState(state)}
function addPwmResource(){
  const templates=waveformTemplates('pwm'),usedEndpoints=new Set(pwms.map(x=>x.endpoint_id)),endpoint=templates.find(x=>!usedEndpoints.has(x.endpoint_id));if(!endpoint)return;
  const base=templates[0]||{},used=occupied(),pin=(endpoint.capable_pins||[]).find(candidate=>!used.has(candidate))||null,index=pwms.length;
  pwms.push({...structuredClone(base),...structuredClone(endpoint),enabled:true,name:`pwm_${index}`,pin,frequency_hz:base.frequency_hz||20000,default_duty_percent:base.default_duty_percent??50,active_low:base.active_low??false});
  renderResources();
}
function addStripResource(){
  const templates=waveformTemplates('ws2812'),usedEndpoints=new Set(pixelStrips.map(x=>x.endpoint_id)),endpoint=templates.find(x=>!usedEndpoints.has(x.endpoint_id));if(!endpoint)return;
  const base=templates[0]||{},used=occupied(),pin=(endpoint.capable_pins||[]).find(candidate=>!used.has(candidate))||null,index=pixelStrips.length;
  pixelStrips.push({...structuredClone(base),...structuredClone(endpoint),enabled:true,name:`strip_${index}`,pin,pixel_count:base.pixel_count||8,max_pixels:base.max_pixels||64,color_order:base.color_order||'GRB',reset_time_us:base.reset_time_us||80});
  renderResources();
}

async function init(){
  catalog=await (await fetch('/api/catalog')).json();$('#boardSelect').innerHTML=catalog.boards.map(x=>`<option value="${x.id}">${x.label}</option>`).join('');loadBoard(catalog.boards[0].id);
  $('#boardSelect').onchange=e=>loadBoard(e.target.value);$('#addAxis').onclick=()=>{if(axes.length>=5)return;axes.push({step:null,dir:null,dir_inverted:false,enable:null,enable_source:null,enable_active_low:true,driver_type:'none',tmc_uart:null,tmc_address:0,limit:null});renderResources()};
  $('#addGpio').onclick=()=>{if(gpios.length>=16)return;gpios.push({name:`gpio_${gpios.length+1}`,pin:null,direction:'input',pull:'none',active_low:false,safe_level:false,debounce_ms:0});renderResources()};
  $('#addUart').onclick=()=>{const used=new Set(uarts.map(x=>x.port));const endpoint=uartTemplates().find(x=>!used.has(x.port));if(!endpoint)return;uarts.push(structuredClone(endpoint));uarts.sort((a,b)=>a.port-b.port);renderResources()};
  $('#addPwm').onclick=addPwmResource;$('#addStrip').onclick=addStripResource;
  $('#exportConfig').onclick=exportManifest;$('#compileConfig').onclick=compileManifest;$('#buildFirmware').onclick=buildFirmware;$('#refresh').onclick=refresh;$('#demoMotion').onclick=()=>{demoRunning=!demoRunning;$('#demoMotion').textContent=demoRunning?'停止演示':'演示运动'};
  await refresh();setInterval(()=>{if(demoRunning&&state?.source==='demo')animateDemo();else refresh()},250);
}
init();
