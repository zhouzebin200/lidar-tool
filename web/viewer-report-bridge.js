// Runs in the original viewer; the modal iframe never stops its WebSocket.
const reportPanel=$('reportStudioPanel'),reportFrame=$('reportStudioFrame');
$('openReportStudio').onclick=()=>{if(!reportFrame.getAttribute('src'))reportFrame.src='/report.html?embedded=1';reportPanel.showModal();};
$('closeReportStudio').onclick=()=>reportPanel.close();
window.addEventListener('message',event=>{
 if(event.origin!==location.origin||event.source!==reportFrame.contentWindow||event.data?.type!=='lijian:capture-views')return;
 const reply=data=>reportFrame.contentWindow.postMessage({type:'lijian:views',...data},location.origin);
 if(!rendererReady||!state.points||!state.lastFrameAt||performance.now()-state.lastFrameAt>2000){reply({error:'当前没有有效点云。请先连接雷达或回放PCAP，确认点云显示后再采集。'});return;}
 if(Number(event.data.deviceId)!==state.activeDevice){reply({error:'报告所选设备与当前点云来源不一致，请在报告中选择当前点云对应的雷达。'});return;}
 const oldYaw=state.yaw,oldPitch=state.pitch;
 try{
  // Synchronous rendering captures the same point buffer for all three views.
  const views=[['斜视',-.74,.56],['侧视一',0,0],['侧视二',Math.PI/2,0]].map(([role,yaw,pitch])=>{state.yaw=yaw;state.pitch=pitch;render(false);const src=canvas.toDataURL('image/png');return{role,name:role+'点云截图.png',src,marks:[]};});
  reply({photos:views,deviceId:state.activeDevice,points:state.points,fps:$('fps').textContent,capturedAt:new Date().toLocaleString()});
 }catch(error){reply({error:'点云截图失败：'+error.message});}
 finally{state.yaw=oldYaw;state.pitch=oldPitch;render(false);}
});
