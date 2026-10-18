import type cytoscape from 'cytoscape';

export type PhysicsSettings={repulsion:number;linkLength:number;spacing:number;gravity:number;damping:number;nodeScale:number};
type Body={node:cytoscape.NodeSingular;x:number;y:number;vx:number;vy:number;fx:number;fy:number;radius:number};

/** Persistent, damped spring/charge simulation. All coordinates are model-space. */
export class LivePhysics {
  private bodies:Body[]=[];
  private links:[Body,Body][]=[];
  private velocities=new Map<string,{vx:number;vy:number}>();
  private frame=0;
  private previous=0;
  private frames=0;
  private measuredAt=0;
  private disposed=false;
  running=true;
  visible=true;

  constructor(private cy:cytoscape.Core,private settings:()=>PhysicsSettings,private report:(fps:number)=>void){
    this.frame=requestAnimationFrame(this.tick);
  }
  sync(){
    const byId=new Map<string,Body>();
    this.bodies=this.cy.nodes().map(node=>{
      const p=node.position(),v=this.velocities.get(node.id())??{vx:0,vy:0};
      const b={node,x:p.x,y:p.y,...v,fx:0,fy:0,radius:node.width()/2};byId.set(node.id(),b);return b;
    });
    this.links=[];const seen=new Set<string>();
    this.cy.edges().forEach(edge=>{
      const from=edge.source().id(),to=edge.target().id(),key=[from,to].sort().join('|');
      if(from===to||seen.has(key))return;seen.add(key);
      const a=byId.get(from),b=byId.get(to);if(a&&b)this.links.push([a,b]);
    });
    for(const id of this.velocities.keys())if(!byId.has(id))this.velocities.delete(id);
  }
  private tick=(now:number)=>{
    if(this.disposed)return;
    const dt=Math.min(1.8,Math.max(.2,(now-(this.previous||now-16.67))/16.67));this.previous=now;
    if(this.running&&this.visible&&document.visibilityState!=='hidden'&&this.bodies.length){
      const v=this.settings(),bs=this.bodies;
      for(const b of bs){const p=b.node.position();b.x=p.x;b.y=p.y;b.fx=-p.x*v.gravity*.001;b.fy=-p.y*v.gravity*.001;b.radius=b.node.width()/2;}
      for(let i=0;i<bs.length;i++)for(let j=i+1;j<bs.length;j++){
        const a=bs[i],b=bs[j];let dx=b.x-a.x,dy=b.y-a.y;
        if(Math.abs(dx)+Math.abs(dy)<.01){dx=Math.cos(i+j+1);dy=Math.sin(i+j+1);}
        const distance=Math.max(1,Math.hypot(dx,dy)),ux=dx/distance,uy=dy/distance;
        const overlap=Math.max(0,a.radius+b.radius+20-distance);
        const force=Math.min(12,v.repulsion/(distance*distance)*.55)+overlap*.08;
        a.fx-=ux*force;a.fy-=uy*force;b.fx+=ux*force;b.fy+=uy*force;
      }
      for(const [a,b] of this.links){
        const dx=b.x-a.x,dy=b.y-a.y,distance=Math.max(1,Math.hypot(dx,dy));
        const force=(distance-v.linkLength*v.spacing-a.radius-b.radius)*.012;
        const fx=dx/distance*force,fy=dy/distance*force;a.fx+=fx;a.fy+=fy;b.fx-=fx;b.fy-=fy;
      }
      const drag=Math.pow(v.damping,dt);
      this.cy.batch(()=>{
        for(const b of bs){
          if(b.node.locked()||b.node.grabbed()){b.vx=0;b.vy=0;continue;}
          b.vx=(b.vx+b.fx*dt)*drag;b.vy=(b.vy+b.fy*dt)*drag;
          const speed=Math.hypot(b.vx,b.vy);if(speed>24){b.vx*=24/speed;b.vy*=24/speed;}
          b.node.position({x:b.x+b.vx*dt,y:b.y+b.vy*dt});
          this.velocities.set(b.node.id(),{vx:b.vx,vy:b.vy});
        }
      });
      this.frames++;
    }
    if(now-this.measuredAt>600){this.report(Math.round(this.frames*1000/(now-(this.measuredAt||now-600))));this.frames=0;this.measuredAt=now;}
    this.frame=requestAnimationFrame(this.tick);
  };
  destroy(){this.disposed=true;cancelAnimationFrame(this.frame);}
}

/** Shortest unweighted, undirected connection in the currently displayed graph. */
export function displayedPath(cy:cytoscape.Core,from:string,to:string):string[]|null {
  if(!cy.hasElementWithId(from)||!cy.hasElementWithId(to))return null;
  const parents=new Map<string,{node:string;edge:string}>(),queue=[from],seen=new Set([from]);
  for(let i=0;i<queue.length;i++){
    const id=queue[i];if(id===to){
      const path=[id];let current=id;
      while(current!==from){const parent=parents.get(current)!;path.unshift(parent.node,parent.edge);current=parent.node;}
      return path;
    }
    cy.getElementById(id).connectedEdges().forEach(edge=>{
      const next=edge.source().id()===id?edge.target().id():edge.source().id();
      if(!seen.has(next)){seen.add(next);parents.set(next,{node:id,edge:edge.id()});queue.push(next);}
    });
  }
  return null;
}
