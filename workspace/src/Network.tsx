import {useEffect,useMemo,useRef,useState} from 'react';
import cytoscape from 'cytoscape';
import {graphElements,records,colors,type Answer,type RecordNode} from './data';
import {LivePhysics,displayedPath} from './live-graph';
import './network.css';
import './exploration.css';

const layouts = [
  {id:'cose',name:'Force-directed',hint:'Live spring/charge simulation. Drag nodes to pin them; parameters affect the next frame.'},
  {id:'concentric',name:'Radial',hint:'Concentric rings group nodes by their number of displayed neighbors.'},
  {id:'breadthfirst',name:'Hierarchical',hint:'Breadth-first layers show connection distance from the focus node; not causality.'},
  {id:'circle',name:'Circle',hint:'An even circle makes cross-connections easier to inspect.'},
  {id:'grid',name:'Grid',hint:'A regular grid gives each entity equal visual space.'},
];
const graphKinds = new Set(['Organisation','Domain','Idea','Event','Claim','Judgment']);
const defaultView={spacing:1.2,repulsion:9000,linkLength:85,nodeScale:1,labelSize:14,edgeOpacity:.6,labels:'smart',nodeLimit:36,gravity:1,damping:.84,degreeSizing:false};
type ViewSettings=typeof defaultView;

function layoutOptions(name:string,graph:cytoscape.Core,view:ViewSettings):cytoscape.LayoutOptions {
  const side=Math.max(600,Math.sqrt(graph.nodes().length)*90);
  const common={name,fit:false,animate:!matchMedia('(prefers-reduced-motion: reduce)').matches,animationDuration:450,animationEasing:'ease-in-out' as const,padding:35,nodeDimensionsIncludeLabels:false,boundingBox:{x1:-side/2,y1:-side/2,w:side,h:side}};
  switch(name){
    case 'cose': return {...common,name:'cose',randomize:false,nodeRepulsion:()=>view.repulsion,idealEdgeLength:()=>view.linkLength*view.spacing};
    case 'concentric': return {...common,name:'concentric',minNodeSpacing:35*view.spacing,concentric:n=>n.neighborhood('node').length,levelWidth:()=>2};
    case 'breadthfirst': return {...common,name:'breadthfirst',directed:false,spacingFactor:view.spacing,roots:graph.nodes().filter(n=>!!n.data('anchor')).map(n=>n.id())};
    case 'circle': return {...common,name:'circle',avoidOverlap:false,radius:Math.max(side/2,graph.nodes().length*5),spacingFactor:view.spacing};
    default: return {...common,name:'grid',condense:false,spacingFactor:view.spacing};
  }
}

export function Network({answer,focus,select,focusNode}:{answer:Answer|null;focus:string|null;select:(id:string)=>void;focusNode:(id:string|null)=>void}) {
  const stage=useRef<HTMLDivElement>(null),canvas=useRef<HTMLDivElement>(null),engine=useRef<cytoscape.Core|null>(null);
  const searchInput=useRef<HTMLInputElement>(null),fullscreenButton=useRef<HTMLButtonElement>(null),nativeFullscreen=useRef(false);
  const savedCamera=useRef<{zoom:number;pan:cytoscape.Position}|null>(null);
  const selectRef=useRef(select);selectRef.current=select;
  const [expanded,setExpanded]=useState(false),[searchOpen,setSearchOpen]=useState(false),[term,setTerm]=useState('');
  const [layout,setLayout]=useState('cose'),[scope,setScope]=useState('neighborhood'),[picked,setPicked]=useState<string|null>(null);
  const [settingsOpen,setSettingsOpen]=useState(false),[view,setView]=useState(defaultView);
  const viewRef=useRef(view);viewRef.current=view;
  const simulation=useRef<LivePhysics|null>(null),initialized=useRef(false);
  const [paused,setPaused]=useState(()=>matchMedia('(prefers-reduced-motion: reduce)').matches),[fps,setFps]=useState(0),[pins,setPins]=useState<string[]>([]);
  const [hiddenKinds,setHiddenKinds]=useState<string[]>([]),[pathStart,setPathStart]=useState<string|null>(null),[pathEnd,setPathEnd]=useState<string|null>(null);
  const [pathIds,setPathIds]=useState<string[]|null>(null),[pathOpen,setPathOpen]=useState(false);
  const {nodes}=useMemo(()=>records(answer),[answer]);
  const entities=useMemo(()=>[...nodes.values()].filter(n=>graphKinds.has(n.kind)).sort((a,b)=>a.label.localeCompare(b.label)),[nodes]);
  const matches=useMemo(()=>{
    const words=term.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
    return entities.filter(n=>words.every(word=>(n.label+' '+n.kind).toLocaleLowerCase().includes(word)));
  },[entities,term]);
  const graph=useMemo(()=>{
    const base=graphElements(answer,focus,scope==='all',view.nodeLimit);
    const ids=new Set(base.elements.filter(e=>'kind' in e.data&&!hiddenKinds.includes(e.data.kind)).map(e=>e.data.id));
    return {...base,shown:ids.size,elements:base.elements.filter(e=>'source' in e.data?ids.has(e.data.source)&&ids.has(e.data.target):ids.has(e.data.id))};
  },[answer,focus,scope,view.nodeLimit,hiddenKinds]);
  const chosen=picked?nodes.get(picked):undefined;
  const visibleNodes=useMemo(()=>{
    const ids=new Set(graph.elements.filter(e=>'kind' in e.data).map(e=>e.data.id));return entities.filter(n=>ids.has(n.id));
  },[graph,entities]);
  const algorithm=layouts.find(item=>item.id===layout)!;

  useEffect(()=>{setPicked(null);setTerm('');setPathStart(null);setPathEnd(null);},[answer]);
  useEffect(()=>{if(focus)setScope('neighborhood');},[focus]);
  useEffect(()=>{if(searchOpen)searchInput.current?.focus();},[searchOpen]);

  useEffect(()=>{
    if(!canvas.current)return;
    const cy=cytoscape({container:canvas.current,elements:[],minZoom:.08,maxZoom:4,layout:{name:'preset'}});
    engine.current=cy;
    cy.on('tap','node',event=>{const id=event.target.id();setPicked(id);selectRef.current(id);});
    cy.on('mouseover','node',event=>{
      cy.elements().addClass('hover-muted');event.target.closedNeighborhood().removeClass('hover-muted').addClass('neighbor');
    });
    cy.on('mouseout','node',()=>cy.elements().removeClass('hover-muted neighbor'));
    cy.on('dragfree','node',event=>{event.target.lock().addClass('pinned');setPins(cy.nodes(':locked').map(n=>n.id()));});
    cy.on('zoom',()=>cy.style().update());
    simulation.current=new LivePhysics(cy,()=>viewRef.current,setFps);
    const visibility=new IntersectionObserver(([entry])=>{if(simulation.current)simulation.current.visible=entry.isIntersecting;});visibility.observe(canvas.current);
    const observer=new ResizeObserver(()=>cy.resize());
    observer.observe(canvas.current);
    return()=>{observer.disconnect();visibility.disconnect();simulation.current?.destroy();cy.destroy();engine.current=null;};
  },[]);

  useEffect(()=>{
    const cy=engine.current;if(!cy)return;
    const keep=new Set(graph.elements.map(e=>e.data.id));
    cy.batch(()=>{
      cy.elements().filter(e=>!keep.has(e.id())).remove();
      for(const element of graph.elements){
        const existing=cy.getElementById(element.data.id);
        if(existing.length){existing.data(element.data);continue;}
        const node=cy.add(element);
        if(node.isNode()){
          const i=cy.nodes().length-1,angle=i*2.39996,radius='anchor' in element.data&&element.data.anchor?0:40*Math.sqrt(i+1);
          node.position({x:Math.cos(angle)*radius,y:Math.sin(angle)*radius});
        }
      }
    });
    simulation.current?.sync();setPins(cy.nodes(':locked').map(n=>n.id()));
    if(!initialized.current&&cy.nodes().length){initialized.current=true;cy.resize();cy.fit(undefined,45);}
  },[graph]);

  useEffect(()=>{
    const cy=engine.current;if(!cy)return;
    const size=(n:cytoscape.NodeSingular)=>view.nodeScale*(view.degreeSizing?18+Math.sqrt(n.neighborhood('node').length)*7:Number(n.data('size')));
    cy.style([
      {selector:'node',style:{'background-color':'data(color)',width:size,height:size,
        'font-size':()=>view.labelSize/cy.zoom(),label:(n:cytoscape.NodeSingular)=>{
          if(view.labels==='none')return '';
          if(view.labels==='smart'&&cy.zoom()<.65&&!n.data('anchor')&&!n.selected()&&!n.hasClass('neighbor'))return '';
          return n.data(view.labels==='full'?'fullLabel':'label');
        },'text-max-width':'230px','text-wrap':view.labels==='full'?'wrap':'none',color:'#cad9e8','text-valign':'bottom','text-margin-y':8,'text-outline-color':'#101927','text-outline-width':2}},
      {selector:'edge',style:{width:1.4,'line-color':'#52738e','target-arrow-color':'#52738e','target-arrow-shape':'triangle','curve-style':'bezier',opacity:view.edgeOpacity}},
      {selector:'.pinned',style:{'border-width':3,'border-color':'#e7bd73','border-style':'dashed'}},
      {selector:'node:selected',style:{'border-width':3,'border-color':'#d3ffeb'}},
      {selector:'.search-match',style:{'border-width':3,'border-color':'#f1d189'}},
      {selector:'.search-muted,.hover-muted,.path-muted',style:{opacity:.12}},
      {selector:'.neighbor',style:{'z-index':10}},
      {selector:'edge.neighbor',style:{'line-color':'#a4dbc9','target-arrow-color':'#a4dbc9',width:2.4}},
      {selector:'.path-active',style:{opacity:1,'z-index':20}},
      {selector:'edge.path-active',style:{'line-color':'#f1d189','target-arrow-color':'#f1d189',width:3}},
      {selector:'node.path-active',style:{'border-color':'#f1d189','border-width':3}},
    ]);
  },[graph,view.nodeScale,view.labelSize,view.labels,view.edgeOpacity,view.degreeSizing]);

  useEffect(()=>{
    const cy=engine.current;if(!cy)return;
    if(simulation.current)simulation.current.running=layout==='cose'&&!paused;
    cy.nodes().stop(true,false);
    if(layout==='cose')return;
    const active=cy.layout(layoutOptions(layout,cy,view));active.run();
    return()=>{active.stop();};
  },[graph,layout,paused,view.spacing]);

  useEffect(()=>{
    const cy=engine.current;if(!cy)return;
    cy.elements().removeClass('path-active path-muted');
    if(!pathStart||!pathEnd){setPathIds(null);return;}
    const path=displayedPath(cy,pathStart,pathEnd);setPathIds(path);
    if(path){cy.elements().addClass('path-muted');for(const id of path)cy.getElementById(id).removeClass('path-muted').addClass('path-active');}
  },[graph,pathStart,pathEnd]);

  useEffect(()=>{
    const cy=engine.current;if(!cy)return;
    cy.elements().removeClass('search-match search-muted');
    if(searchOpen&&term.trim()){
      const ids=new Set(matches.map(n=>n.id));
      cy.nodes().forEach(n=>{n.addClass(ids.has(n.id())?'search-match':'search-muted');});
      cy.edges().addClass('search-muted');
    }
    cy.nodes().unselect();if(picked)cy.getElementById(picked).select();
  },[matches,searchOpen,term,picked,graph,layout]);

  async function exitFullscreen(){
    if(document.fullscreenElement===stage.current)await document.exitFullscreen().catch(()=>{});
    setExpanded(false);
    fullscreenButton.current?.focus();
  }
  async function toggleFullscreen(){
    if(expanded){await exitFullscreen();return;}
    const cy=engine.current;if(cy)savedCamera.current={zoom:cy.zoom(),pan:{...cy.pan()}};
    setExpanded(true);
    try{await stage.current?.requestFullscreen?.();}catch{/* Viewport-filling mode also works when browser fullscreen is unavailable. */}
  }
  useEffect(()=>{
    const onChange=()=>{
      if(document.fullscreenElement===stage.current)nativeFullscreen.current=true;
      else if(nativeFullscreen.current){nativeFullscreen.current=false;setExpanded(false);fullscreenButton.current?.focus();}
    };
    document.addEventListener('fullscreenchange',onChange);
    return()=>document.removeEventListener('fullscreenchange',onChange);
  },[]);
  useEffect(()=>{
    if(!expanded)return;
    const overflow=document.body.style.overflow;document.body.style.overflow='hidden';
    stage.current?.focus();
    const frame=requestAnimationFrame(()=>{engine.current?.resize();engine.current?.fit(undefined,80);});
    return()=>{
      cancelAnimationFrame(frame);document.body.style.overflow=overflow;
      requestAnimationFrame(()=>{const cy=engine.current;if(cy&&savedCamera.current){cy.resize();cy.viewport(savedCamera.current);}});
    };
  },[expanded]);
  useEffect(()=>{
    const onKey=(event:KeyboardEvent)=>{
      if(!expanded&&!stage.current?.contains(document.activeElement))return;
      const editable=(event.target as HTMLElement)?.matches('input,textarea,select,[contenteditable="true"]');
      if(event.key==='Escape'){
        if(expanded){event.preventDefault();void exitFullscreen();}
        else if(searchOpen){event.preventDefault();setSearchOpen(false);}
      }else if((event.key==='/'&&!editable)||((event.ctrlKey||event.metaKey)&&event.key==='k')){
        event.preventDefault();setSearchOpen(true);searchInput.current?.focus();
      }else if(event.key==='Tab'&&expanded){
        const controls=stage.current?.querySelectorAll<HTMLElement>('button:not(:disabled),input,select,[tabindex="0"]');
        if(!controls?.length)return;
        const first=controls[0],last=controls[controls.length-1];
        if(event.shiftKey&&(document.activeElement===first||document.activeElement===stage.current)){event.preventDefault();last.focus();}
        else if(!event.shiftKey&&document.activeElement===last){event.preventDefault();first.focus();}
      }
    };
    window.addEventListener('keydown',onKey);
    return()=>window.removeEventListener('keydown',onKey);
  },[expanded,searchOpen]);

  function reveal(node:RecordNode){
    setHiddenKinds(kinds=>kinds.filter(kind=>kind!==node.kind));
    setScope('neighborhood');focusNode(node.id);setPicked(node.id);select(node.id);
    setSearchOpen(false);stage.current?.focus();
  }
  function pinSelected(){
    const cy=engine.current;if(!cy||!picked)return;const node=cy.getElementById(picked);
    if(node.locked())node.unlock().removeClass('pinned');else node.lock().addClass('pinned');
    setPins(cy.nodes(':locked').map(n=>n.id()));
  }
  function releasePins(){const cy=engine.current;if(cy){cy.nodes().unlock().removeClass('pinned');setPins([]);}}
  function toggleLayer(kind:string){setHiddenKinds(kinds=>kinds.includes(kind)?kinds.filter(k=>k!==kind):[...kinds,kind]);}
  function exportPng(){
    const cy=engine.current;if(!cy)return;
    const a=document.createElement('a');a.href=cy.png({full:true,bg:'#101927',maxWidth:3000,maxHeight:3000});a.download='axthena-graph.png';a.click();
  }

  return <div ref={stage} className={`network-stage${expanded?' expanded':''}`} role={expanded?'dialog':'region'} aria-modal={expanded||undefined} aria-label="Graph explorer" tabIndex={0}>
    <div className="network-toolbar">
      {expanded&&<strong className="network-brand">◈ Axthena <span>Graph explorer</span></strong>}
      <label className="layout-picker"><span>Layout</span><select aria-label="Graph layout" value={layout} onChange={e=>setLayout(e.target.value)}>{layouts.map(item=><option key={item.id} value={item.id}>{item.name}</option>)}</select></label>
      <select aria-label="Graph display scope" value={scope} onChange={e=>{setScope(e.target.value);focusNode(null);setView(v=>({...v,nodeLimit:e.target.value==='all'?Math.max(12,Math.min(entities.length,500)):36}));}}><option value="neighborhood">Neighborhood</option><option value="all">All returned · max 500</option></select>
      <button aria-expanded={searchOpen} aria-controls="graph-search-panel" onClick={()=>{setSearchOpen(!searchOpen);setPathOpen(false);}}>{searchOpen?'Hide search':'Search'} <kbd>/</kbd></button>
      <button aria-expanded={settingsOpen} aria-controls="graph-view-settings" onClick={()=>setSettingsOpen(!settingsOpen)}>{settingsOpen?'Hide settings':'View settings'}</button>
      <button className={pathOpen?'active':''} aria-expanded={pathOpen} onClick={()=>{setPathOpen(!pathOpen);setSearchOpen(false);}}>Explore paths</button>
      {layout==='cose'&&<button className="physics-toggle" onClick={()=>setPaused(!paused)}>{paused?'▶ Resume physics':'Ⅱ Pause physics'}</button>}
      {focus&&<button onClick={()=>{focusNode(null);setPicked(null);setTerm('');}}>Reset focus</button>}
      <button ref={fullscreenButton} className="fullscreen-button" onClick={()=>void toggleFullscreen()} aria-label={expanded?'Exit fullscreen graph':'Fullscreen graph'}>{expanded?'↙ Exit fullscreen':'⛶ Fullscreen'}</button>
    </div>
    <div className="network-surface">
      <div ref={canvas} className="network" aria-label="Interactive evidence graph"/>
      {searchOpen&&<section id="graph-search-panel" className="graph-search" aria-label="Search graph entities">
        <div className="graph-search-input"><input ref={searchInput} type="search" aria-label="Search graph" value={term} onChange={e=>setTerm(e.target.value)} placeholder="Find a company, activity or idea…" onKeyDown={e=>{if(e.key==='Enter'&&matches[0]){e.preventDefault();reveal(matches[0]);}}}/><button aria-label="Close graph search" onClick={()=>{setSearchOpen(false);stage.current?.focus();}}>×</button></div>
        <small role="status">{matches.length} matches in {entities.length} returned entities · select to reveal</small>
        <div className="graph-search-results">{matches.slice(0,40).map(node=><button key={node.id} onClick={()=>reveal(node)}><i style={{background:colors[node.kind]}}/><span>{node.label}<small>{node.kind==='Domain'?'Activity category':node.kind}</small></span></button>)}{!matches.length&&<p>No matching entities in this query result.</p>}</div>
        {matches.length>40&&<small>Showing the first 40 matches. Type more to narrow them.</small>}
      </section>}
      {settingsOpen&&<section id="graph-view-settings" className="graph-view-settings" aria-label="Live graph view settings">
        <div className="view-settings-heading"><strong>View settings</strong><button aria-label="Close view settings" onClick={()=>setSettingsOpen(false)}>×</button></div>
        <small>Live display controls · {algorithm.hint}</small>
        <div className="layer-controls" aria-label="Entity layers">{[...graphKinds].map(kind=><button key={kind} aria-pressed={!hiddenKinds.includes(kind)} onClick={()=>toggleLayer(kind)}><i style={{background:colors[kind]}}/>{kind==='Domain'?'Activities':kind}</button>)}</div>
        {([
          ['nodeLimit','Node limit',12,500,1,''],['nodeScale','Node size',.5,2,.1,'×'],
          ['labelSize','Label size',8,20,1,'px'],['edgeOpacity','Edge opacity',.1,1,.05,''],
          ['spacing','Layout spacing',.6,2.4,.1,'×'],
          ...(layout==='cose'?[['repulsion','Force strength',1000,30000,500,''],['linkLength','Link length',30,200,5,''],['gravity','Center gravity',0,4,.1,''],['damping','Momentum retention',.5,.95,.01,'']]:[]),
        ] as [keyof ViewSettings,string,number,number,number,string][]).map(([key,label,min,max,step,unit])=><label className="view-slider" key={key}><span>{label}<output>{Number(view[key]).toLocaleString(undefined,{maximumFractionDigits:2})}{unit}</output></span><input aria-label={label} type="range" min={min} max={max} step={step} value={Number(view[key])} onChange={e=>setView(v=>({...v,[key]:Number(e.target.value)}))}/></label>)}
        <label className="view-labels">Node labels<select aria-label="Node labels" value={view.labels} onChange={e=>setView(v=>({...v,labels:e.target.value}))}><option value="smart">Smart zoom labels</option><option value="short">Short labels</option><option value="full">Full labels</option><option value="none">Hidden</option></select></label>
        <label className="degree-toggle"><input type="checkbox" checked={view.degreeSizing} onChange={e=>setView(v=>({...v,degreeSizing:e.target.checked}))}/> Size by displayed degree</label>
        <button className="view-reset" onClick={()=>{setView({...defaultView,nodeLimit:scope==='all'?Math.max(12,Math.min(entities.length,500)):36});setHiddenKinds([]);}}>Reset view settings</button>
        <button className="view-reset" onClick={releasePins} disabled={!pins.length}>Release {pins.length} pinned nodes</button>
      </section>}
      {pathOpen&&<section className="graph-path-panel" aria-label="Displayed path explorer">
        <div className="view-settings-heading"><strong>Find a connection</strong><button aria-label="Close path explorer" onClick={()=>setPathOpen(false)}>×</button></div>
        <small>Shortest undirected path in the displayed projection. Connections indicate source relevance, not causality.</small>
        <label>From<select aria-label="Path start" value={pathStart??''} onChange={e=>setPathStart(e.target.value||null)}><option value="">Choose an entity…</option>{visibleNodes.map(n=><option key={n.id} value={n.id}>{n.label}</option>)}</select></label>
        <label>To<select aria-label="Path end" value={pathEnd??''} onChange={e=>setPathEnd(e.target.value||null)}><option value="">Choose an entity…</option>{visibleNodes.map(n=><option key={n.id} value={n.id}>{n.label}</option>)}</select></label>
        {pathStart&&pathEnd&&<div role="status" className="path-result">{pathIds?`${(pathIds.length-1)/2} connections`:'No path in this displayed scope. Try All returned or enable other layers.'}</div>}
        {pathIds&&<ol className="path-steps">{pathIds.map((id,i)=>i%2===0?<li key={id}><button onClick={()=>{setPicked(id);select(id);}}>{nodes.get(id)?.label}</button></li>:<li className="path-relation" key={id}>↓ {engine.current?.getElementById(id).data('label')}</li>)}</ol>}
        <button className="view-reset" onClick={()=>{setPathStart(null);setPathEnd(null);}}>Clear path highlight</button>
      </section>}
      {expanded&&!searchOpen&&!settingsOpen&&chosen&&<aside className="graph-selection"><span style={{color:colors[chosen.kind]}}>{chosen.kind}</span><p>{chosen.label}</p><button onClick={()=>reveal(chosen)}>Focus neighborhood</button><button onClick={pinSelected}>{pins.includes(chosen.id)?'Unpin position':'Pin position'}</button><button onClick={()=>{setPathStart(chosen.id);setPathEnd(null);setPathOpen(true);}}>Start path here</button><button onClick={()=>void exitFullscreen()}>Inspect evidence ↗</button></aside>}
      {expanded&&<div className="fullscreen-legend">{['Organisation','Idea','Domain','Event','Claim'].map(kind=><button key={kind} aria-pressed={!hiddenKinds.includes(kind)} onClick={()=>toggleLayer(kind)}><i style={{background:colors[kind]}}/>{kind==='Domain'?'Activity':kind}</button>)}</div>}
    </div>
    <div className="network-footer"><div><span className="graph-count">{graph.shown} / {graph.total} returned entities{answer?.results.some(r=>r.truncated)?' · query truncated':''} <span className={`simulation-status${layout==='cose'&&!paused?' running':''}`}>{layout==='cose'?(paused?'PAUSED':`LIVE · ${fps} steps/s`):'ANIMATED LAYOUT'}</span> · {pins.length} pinned</span><small title={algorithm.hint}>{algorithm.name} · {algorithm.hint}</small></div><div className="network-actions"><button onClick={()=>engine.current?.fit(undefined,35)}>Fit</button><button aria-label="Zoom in graph" onClick={()=>{const cy=engine.current;if(cy)cy.zoom({level:cy.zoom()*1.3,renderedPosition:{x:cy.width()/2,y:cy.height()/2}});}}>＋</button><button aria-label="Zoom out graph" onClick={()=>{const cy=engine.current;if(cy)cy.zoom({level:cy.zoom()/1.3,renderedPosition:{x:cy.width()/2,y:cy.height()/2}});}}>−</button><button onClick={exportPng}>PNG</button></div></div>
  </div>;
}
