const assert=require('node:assert/strict');
const {performance}=require('node:perf_hooks');
const fs=require('node:fs');
const p=require('../source/main/web_server/www/eink_pipeline.js');
const width=480,height=720, size=width*height;
const input=new Uint8ClampedArray(size*4);
const palette=new Set([0,1,2,3,5,6]);
const swatches=[[202,137,104],[241,180,140],[164,92,62],[238,45,42],[45,212,95],[39,82,232]];
let seed=12;
for(let y=0;y<height;y++)for(let x=0;x<width;x++){
  const i=(y*width+x)*4;
  let rgb;
  if(y<120) rgb=Array(3).fill(255*x/(width-1));
  else if(y<360) rgb=swatches[(y<240?0:3)+Math.floor(x/160)];
  else if(y<480){seed=(Math.imul(seed,1664525)+1013904223)>>>0;rgb=[105+seed%80,87+seed%60,48+seed%90];}
  else if(y<600)rgb=Array(3).fill(x%39<3||y%19<2?20:236);
  else rgb=x>110&&x<380&&y>617&&y<691?[100,130,145]:[246,246,241];
  input.set([...rgb,255],i);
}
const linear=Uint8Array.from({length:256},(_,i)=>i);
const lin=i=>i<=10?i/3294.6:((i/255+.055)/1.055)**2.4;
const lab=rgb=>p.toLab(...rgb.map(lin));
for(let r=0;r<=1;r+=.2)for(let g=0;g<=1;g+=.2)for(let b=0;b<=1;b+=.2)
  assert.ok(p.fromLab(...p.toLab(r,g,b)).every((v,i)=>Math.abs(v-[r,g,b][i])<1e-4));
function blur(bytes,sigma){
  const n=Math.ceil(3*sigma), w=Array.from({length:2*n+1},(_,i)=>Math.exp(-((i-n)**2)/(2*sigma*sigma)));
  const sum=w.reduce((a,b)=>a+b,0);
  const tmp=new Float32Array(size*3),out=new Float32Array(size*3);
  for(let y=0;y<height;y++)for(let x=0;x<width;x++)for(let c=0;c<3;c++)
    for(let k=-n;k<=n;k++)tmp[(y*width+x)*3+c]+=w[k+n]*lin(bytes[(y*width+Math.max(0,Math.min(width-1,x+k)))*4+c])/sum;
  for(let y=0;y<height;y++)for(let x=0;x<width;x++)for(let c=0;c<3;c++)
    for(let k=-n;k<=n;k++)out[(y*width+x)*3+c]+=w[k+n]*tmp[(Math.max(0,Math.min(height-1,y+k))*width+x)*3+c]/sum;
  const arr=new Float32Array(size*3);
  for(let i=0;i<arr.length;i+=3)arr.set(p.toLab(out[i],out[i+1],out[i+2]),i);
  return arr;
}
const settings={style:'photo',quality:'best',denoise:'balanced',colors:p.PANEL_COLORS};
const base1=blur(input,1),base15=blur(input,1.5);
const stats=[];
const panels=[];
for(const engine of ['legacy','clean','fine','sharp','vivid']){
 const start=performance.now(),q=p.quantize(input,{...settings,engine}),ms=performance.now()-start;
 assert.ok(q.ids.every(v=>palette.has(v)));
 assert.equal(p.makeFrame(q.ids).body.length,172800);
 if(engine==='legacy'&&process.argv[3]){
   const old=require(require('node:path').resolve(process.argv[3])).quantize(input,settings);
   assert.deepEqual(old.ids,q.ids);
 }
 const a=blur(q.preview,1),b=blur(q.preview,1.5);
 let delta1=0,delta15=0,sx=0,sy=0,sxx=0,syy=0,sxy=0,isolated=0;
 const columns=new Float64Array(width);
 for(let i=0;i<size;i++){
   const j=i*3,x=i%width,y=Math.floor(i/width);
   delta1+=Math.hypot(a[j]-base1[j],a[j+1]-base1[j+1],a[j+2]-base1[j+2]);
   delta15+=Math.hypot(b[j]-base15[j],b[j+1]-base15[j+1],b[j+2]-base15[j+2]);
   sx+=a[j];sy+=base1[j];sxx+=a[j]**2;syy+=base1[j]**2;sxy+=a[j]*base1[j];
   if(y<120)columns[x]+=a[j]/120;
   if(x%160>4&&x%160<155&&y>125&&y<354&&Math.abs(y-240)>5){
     const v=q.ids[i]; if([i-1,i+1,i-width,i+width].every(k=>q.ids[k]!==v))isolated++;
   }
 }
 const u=sx/size,v=sy/size;
 const ssim=(2*u*v+.0001)*(2*(sxy/size-u*v)+.0009)/((u*u+v*v+.0001)*(sxx/size-u*u+syy/size-v*v+.0009));
 let bands=0;for(let i=1;i<width-1;i++) bands+=Math.abs(columns[i-1]+columns[i+1]-2*columns[i]);
 stats.push({engine,ms:ms.toFixed(0),delta1:(delta1/size).toFixed(4),delta15:(delta15/size).toFixed(4),
 ssim:ssim.toFixed(4),isolated,band:(bands/(width-2)).toFixed(5)});
 panels.push(q.preview);
}
assert.ok(stats.find(row=>row.engine==='clean').isolated < stats.find(row=>row.engine==='legacy').isolated);
assert.deepEqual(p.quantize(input,{...settings,engine:'clean'}).preview,panels[1]);
assert.throws(()=>p.quantize(input,{...settings,engine:'clean',canceled:()=>true}), {name:'AbortError'});
console.table(stats);
if(process.argv[2]){
 const out=Buffer.alloc(size*panels.length*3);
 for(let y=0;y<height;y++)for(let a=0;a<panels.length;a++)for(let x=0;x<width;x++){
   const k=(y*width+x)*4,j=(y*width*panels.length+a*width+x)*3;
   out[j]=panels[a][k];out[j+1]=panels[a][k+1];out[j+2]=panels[a][k+2];
 }
 fs.writeFileSync(process.argv[2],Buffer.concat([Buffer.from('P6\n'+width*panels.length+' '+height+'\n255\n'),out]));
}
