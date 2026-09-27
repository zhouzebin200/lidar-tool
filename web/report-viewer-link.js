const viewerLink=document.createElement('button');viewerLink.textContent='自动采集点云三视图';viewerLink.className='primary';$('useDevice').after(viewerLink);
const integrated=window.parent!==window;
viewerLink.disabled=!integrated;
if(!integrated){const note=document.createElement('p');note.textContent='请从点云查看器顶部“检测报告”进入，即可自动采集三视图。';viewerLink.after(note);try{const proto=location.protocol==='https:'?'wss:':'ws:';const reportWs=new WebSocket(`${proto}//${location.host}/ws/points`);reportWs.binaryType='arraybuffer';}catch(_e){}}
let captureTimeout;
viewerLink.onclick=()=>{if($('devices').value===''){message('请先读取并选择当前雷达设备');return;}viewerLink.disabled=true;captureTimeout=setTimeout(()=>{viewerLink.disabled=false;message('点云采集超时，请返回查看器确认运行状态');},10000);window.parent.postMessage({type:'lijian:capture-views',deviceId:devices[+$('devices').value].id},location.origin);};
window.addEventListener('message',event=>{if(!integrated||event.origin!==location.origin||event.source!==window.parent||event.data?.type!=='lijian:views')return;clearTimeout(captureTimeout);viewerLink.disabled=false;const data=event.data;if(data.error){message(data.error);return;}const selected=devices[+$('devices').value];if(!selected||selected.id!==data.deviceId){message('所选设备已变化，未写入截图，请重新采集。');return;}for(const sec of state.sections)sec.photos=sec.photos.filter(p=>!p.fromViewer);for(const p of state.sections.flatMap(s=>s.photos))if(viewRoles.slice(1).includes(p.role))p.role='普通图片';state.sections[4].photos.push(...data.photos.map(p=>({...p,fromViewer:true})));upsertGeneratedNote(state.sections[4],`[点云查看器采集 ${data.capturedAt}]\n设备ID：${data.deviceId}；当前帧：${data.points} 点；接收帧率：${data.fps} FPS。三视图来自同一帧；帧率不是机械转速。`,'[点云查看器采集');changed();render();message('已从当前点云自动生成斜视、侧视一、侧视二，九宫格后三格已填入。');});
const baseReadIntegrated=$('read').onclick;
$('read').onclick=async()=>{devices=[];$('devices').innerHTML='<option value="">请选择设备</option>';await baseReadIntegrated();if(devices.length===1){$('devices').value='0';$('useDevice').click();}};
const baseUseIntegrated=$('useDevice').onclick;
$('useDevice').onclick=async()=>{baseUseIntegrated();if($('devices').value==='')return;const d=devices[+$('devices').value];try{const response=await fetch('/api/status');if(!response.ok)throw Error('状态接口不可用');const snapshot=await response.json();if(devices[+$('devices').value]?.id!==d.id)return;const plugins=(snapshot.native_plugins?.loaded||[]).filter(p=>String(p.vendor).toLowerCase()===String(d.vendor).toLowerCase());const stamp=new Date().toLocaleString(),sdkLines=[`[内置查看器状态 ${stamp}]`,`查看器版本：${snapshot.version||'未知'}；数据来源：${snapshot.pcap?.active?'PCAP回放（离线）':'在线输入'}。`],queueLines=[];
for(const p of plugins){sdkLines.push(`解码插件：${p.name||p.vendor}；插件版本：${p.version||'未提供'}；厂商累计解码帧：${p.frames}；解码错误：${p.decode_errors}。`);queueLines.push(`${p.vendor} 插件累计丢弃包：${p.dropped_packets}；丢弃帧：${p.dropped_frames}。`);const instance=(p.instances||[]).find(x=>x.device_id===d.id);if(instance)sdkLines.push('当前设备标定状态：'+JSON.stringify(instance.calibration));}
upsertGeneratedNote(state.sections[3],sdkLines.join('\n'),'[内置查看器状态');if(queueLines.length)upsertGeneratedNote(state.sections[6],`[软件队列统计 ${stamp}]\n${queueLines.join('\n')}\n属于插件全局统计，不是此雷达的网络丢包率。`,'[软件队列统计');const flow=(snapshot.capture?.flows||[]).find(f=>f.source_ip===d.ip);if(flow)upsertGeneratedNote(state.sections[2],`[查看器网络状态 ${stamp}]\n目标IP：${flow.destination_ip}；目标端口：${flow.destination_port}；VLAN：${(flow.vlan_ids||[]).join('/')||'未标记'}。`,'[查看器网络状态');changed();render();message('已从当前程序自动带入设备、网络、SDK及软件队列统计；转速、网络丢包率和履历需有实测或原始资料。');}catch(error){message('设备资料已带入；附加状态读取失败：'+error.message);}};
async function autoLoadViewerDevice(){
 if(!integrated)return;
 message('正在自动识别当前在线雷达并读取 PTC 数据...');
 for(let attempt=0;attempt<10;attempt++){
  await $('read').onclick();
  if(devices.length){
   if(devices.length>1)message('发现多台在线雷达，请选择目标设备；IP 将由查看器自动填写。');
   return;
  }
  await new Promise(resolve=>setTimeout(resolve,700));
 }
 message('暂未发现在线雷达。点云出现后可点击“读取查看器设备”重试。');
}
setTimeout(autoLoadViewerDevice,200);
