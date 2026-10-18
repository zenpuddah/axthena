import {useEffect,useRef,useState} from 'react';
import type {Answer,Plan} from './data';
import './chat.css';

type Turn={role:'user'|'assistant';text:string;answer?:Answer;citations?:{label:string;id:string}[];error?:boolean};
export function Chat({plan,run,model,selected,disabled,onAnswer,onSelect,onBusy}:{plan:Plan;run:string;model:{providerID:string;id:string};selected:string|null;disabled:boolean;onAnswer:(answer:Answer)=>void;onSelect:(id:string)=>void;onBusy:(busy:boolean)=>void}){
 const [turns,setTurns]=useState<Turn[]>([]),[draft,setDraft]=useState(''),[busy,setBusy]=useState(false);
 const bottom=useRef<HTMLDivElement>(null);
 useEffect(()=>{bottom.current?.scrollIntoView({block:'nearest',behavior:'smooth'});},[turns,busy]);
 // A new run is a new evidence context; older answers remain scoped to their run.
 async function send(text=draft){
  if(!text.trim()||busy||disabled)return;
  const history=turns.filter(t=>!t.error).slice(-8).map(t=>({role:t.role,text:t.text.slice(0,2200)+(t.answer?'\nExecuted scope: '+JSON.stringify({run:t.answer.run_id,plan:t.answer.plan})+'\nSource references: '+JSON.stringify(t.citations??[]):'')}));
  setTurns(t=>[...t,{role:'user',text}]);setDraft('');setBusy(true);onBusy(true);
  try{
   const response=await fetch('/api/ask',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({question:text,history,plan,run,model,selected_id:selected??''})});
   const data=await response.json();if(!response.ok)throw new Error(data.error??'Query failed');
   if(data.plan===null){setTurns(t=>[...t,{role:'assistant',text:data.clarification}]);return;}
   const answer=data as Answer;onAnswer(answer);
   const reports=new Set(answer.results.flatMap(r=>r.nodes.filter(n=>n.kind==='Report').map(n=>n.id))).size;
   const findings=answer.findings??[];
   const textReply=findings.length?findings.map(f=>f.text).join('\n\n'):reports?`I ran the ${answer.plan.intent} query and updated the view with ${reports} returned source reports.`:'No eligible records matched this query. The coverage panel explains exclusions; try a broader scope or a different time basis.';
   const ids=[...new Set(findings.flatMap(f=>f.citations))];
   setTurns(t=>[...t,{role:'assistant',text:textReply+(answer.summary_error?'\n\nResults loaded; the AI summary was unavailable.':''),answer,citations:ids.map((id,i)=>({id,label:`Source ${i+1}`}))}]);
  }catch(e){setTurns(t=>[...t,{role:'assistant',text:String(e),error:true}]);}finally{setBusy(false);onBusy(false);}
 }
 return <section className="chat-panel" aria-label="AI investigation chat">
  <div className="chat-heading"><div><span className="spark">✧</span><strong>Ask Axthena</strong><small>Your question → database query → visual evidence</small></div><button onClick={()=>setTurns([])} disabled={busy||!turns.length}>New chat</button></div>
  <div className="chat-messages" role="log" aria-live="polite">
   {!turns.length&&<div className="chat-welcome"><p>What would you like to investigate?</p><small>I’ll query the graph, update the visualization and show the sources. We can narrow it down together.</small><div className="chat-prompts">{['Show NVIDIA’s activities','Compare NVIDIA and Microsoft','What opinions are reported about NVIDIA?'].map(q=><button key={q} disabled={disabled||busy} onClick={()=>void send(q)}>{q} ↗</button>)}</div></div>}
   {turns.map((turn,i)=><article className={`chat-turn ${turn.role}${turn.error?' failed':''}`} key={i}><div className="eyebrow">{turn.role==='user'?'YOU':'AXTHENA'}</div><p>{turn.text}</p>{turn.answer&&<><div className="query-receipt"><span>✓ Queried {turn.answer.plan.intent} · {turn.answer.plan.stocks.join(' + ')||'full corpus'} · {turn.answer.plan.mode}</span><button disabled={busy} onClick={()=>onAnswer(turn.answer!)}>Restore view</button><details><summary>Query details</summary><pre>{JSON.stringify({run:turn.answer.run_id,plan:turn.answer.plan},null,2)}</pre></details></div>{turn.citations?.map(c=><button className="chat-citation" key={c.id} onClick={()=>{onAnswer(turn.answer!);onSelect(c.id);}}>{c.label} ↗</button>)}{!!turn.answer.findings?.length&&<small>AI interpretation of a retrieved sample. Inspect cited passages.</small>}</>}</article>)}
   {busy&&<div className="chat-working" role="status"><span/> Querying the graph and reading the evidence…</div>}<div ref={bottom}/>
  </div>
  <form className="chat-composer" onSubmit={e=>{e.preventDefault();void send();}}><textarea aria-label="Message Axthena" placeholder={disabled?'Chat needs the connected local workspace':'Ask a question, or follow up: “Only Microsoft”, “Show the source”…'} value={draft} maxLength={4000} onChange={e=>setDraft(e.target.value)} onKeyDown={e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();void send();}}} rows={2}/><button className="primary" disabled={busy||disabled||!draft.trim()}>{busy?'Querying…':'Send ↑'}</button></form>
  <div className="chat-scope">{selected?'Selected graph record included · ':''}{model.providerID}/{model.id} · run {run.slice(4,16)} · follow-ups keep the current scope</div>
 </section>;
}
