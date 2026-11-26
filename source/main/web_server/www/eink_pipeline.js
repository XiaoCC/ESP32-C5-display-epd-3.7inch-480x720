/* 浏览器六色流程：sRGB 转线性光、降噪及色域投影、就近选墨色和可调扩散。
 * 预览和打包使用同一组墨色 ID，校准 RGB 仅模拟面板观感，不修改驱动 LUT。
 * 浏览器优先使用 Worker，环境不支持时回退到分段执行。 */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.EinkPipeline = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const WIDTH = 480, HEIGHT = 720, FRAME_BYTES = WIDTH * HEIGHT / 2;
  const FORMAT = 'otp6-v1';
  let panelRotation = 0, rotationEpoch = 1;
  function setPanelRotation(degrees, epoch = rotationEpoch) {
    if (![0,90,180,270].includes(degrees)) throw new Error('不支持的整屏安装方向');
    panelRotation = degrees; rotationEpoch = epoch;
  }
  function sourceIndex(x, y) {
    return panelRotation === 90 ? (WIDTH-1-x)*HEIGHT+y : panelRotation === 270 ? x*HEIGHT+HEIGHT-1-y : panelRotation === 180 ? (HEIGHT - 1 - y) * WIDTH + WIDTH - 1 - x : y * WIDTH + x;
  }

  const COLORS = Object.freeze([
    { id: 0, name: '黑', css: '#111412' },
    { id: 1, name: '白', css: '#f4f3e8' },
    { id: 2, name: '黄', css: '#e7c83f' },
    { id: 3, name: '红', css: '#c63b35' },
    { id: 5, name: '蓝', css: '#315eae' },
    { id: 6, name: '绿', css: '#398451' },
  ].map(Object.freeze));
  const clamp = (x, lo, hi) => Math.max(lo, Math.min(hi, x));
  const linear = Float32Array.from({ length: 256 }, (_, i) => {
    const v = i / 255;
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  });
  function buildPalette(colors) { return COLORS.map((c, i) => {
    c = {...c, css: colors?.[i] || c.css};
    const rgb = [1, 3, 5].map(i => parseInt(c.css.slice(i, i + 2), 16));
    const lin = rgb.map(v => linear[v]);
    return { ...c, rgb, lin, luminance: .2126 * lin[0] + .7152 * lin[1] + .0722 * lin[2] };
  }); }
  const palette = buildPalette();
  const validInk = id => Number.isInteger(id) && (id === 0 || id === 1 || id === 2 || id === 3 || id === 5 || id === 6);
  const MODES = Object.freeze({ fast: .55, standard: .85, best: 1 });
  const canceledError = () => Object.assign(new Error('预览已更新'), { name: 'AbortError' });

  // Project unrepresentable source colors to the six-ink linear RGB hull.
  // This removes impossible residuals BEFORE diffusion; representable mixtures
  // retain full error gain. Coordinates use the same weights as ink selection.
  function sixInkHull(style, palette) {
    const w=(style==='illustration'?[1,1,1]:[.3,.5,.2]).map(Math.sqrt);
    const points=palette.map(c=>c.lin.map((v,i)=>v*w[i])),faces=[];
    for(let i=0;i<6;i++)for(let j=i+1;j<6;j++)for(let k=j+1;k<6;k++) {
      const a=points[i],b=points[j],c=points[k],u=b.map((v,n)=>v-a[n]),v=c.map((v,n)=>v-a[n]);
      let normal=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
      const norm=Math.hypot(...normal);if(norm<1e-10)continue;
      normal=normal.map(v=>v/norm);
      const sides=points.map(p=>normal.reduce((sum,v,n)=>sum+v*(p[n]-a[n]),0));
      if(Math.min(...sides)<-1e-8&&Math.max(...sides)>1e-8)continue;
      if(Math.max(...sides)>1e-8)normal=normal.map(v=>-v);
      const dot=(a,b)=>a.reduce((sum,v,i)=>sum+v*b[i],0);
      const uu=dot(u,u),uv=dot(u,v),vv=dot(v,v);
      const edges=[[a,b],[b,c],[c,a]].map(([a,b])=>{const d=b.map((v,i)=>v-a[i]);return {a,d,n:dot(d,d)};});
      faces.push({a,u,v,normal,uu,uv,vv,det:uu*vv-uv*uv,edges});
    }
    return {w,faces};
  }
  function projectSixColor(r,g,b,hull,out) {
    const x=r*hull.w[0],y=g*hull.w[1],z=b*hull.w[2];
    let outside=false;
    for(const f of hull.faces) {
      if(f.normal[0]*(x-f.a[0])+f.normal[1]*(y-f.a[1])+f.normal[2]*(z-f.a[2])>1e-8){outside=true;break;}
    }
    if(!outside){out[0]=r;out[1]=g;out[2]=b;return;}
    let distance=Infinity,bx=x,by=y,bz=z;
    for(const f of hull.faces) {
      const h=f.normal[0]*(x-f.a[0])+f.normal[1]*(y-f.a[1])+f.normal[2]*(z-f.a[2]);
      const qx=x-h*f.normal[0],qy=y-h*f.normal[1],qz=z-h*f.normal[2];
      const tx=qx-f.a[0],ty=qy-f.a[1],tz=qz-f.a[2];
      const tu=tx*f.u[0]+ty*f.u[1]+tz*f.u[2],tv=tx*f.v[0]+ty*f.v[1]+tz*f.v[2];
      const s=(tu*f.vv-tv*f.uv)/f.det,t=(tv*f.uu-tu*f.uv)/f.det;
      if(s>=0&&t>=0&&s+t<=1) {
        const d=(x-qx)**2+(y-qy)**2+(z-qz)**2;
        if(d<distance){distance=d;bx=qx;by=qy;bz=qz;}
      } else for(const e of f.edges) {
        const u=clamp(((x-e.a[0])*e.d[0]+(y-e.a[1])*e.d[1]+(z-e.a[2])*e.d[2])/e.n,0,1);
        const qx=e.a[0]+u*e.d[0],qy=e.a[1]+u*e.d[1],qz=e.a[2]+u*e.d[2];
        const d=(x-qx)**2+(y-qy)**2+(z-qz)**2;
        if(d<distance){distance=d;bx=qx;by=qy;bz=qz;}
      }
    }
    out[0]=bx/hull.w[0];out[1]=by/hull.w[1];out[2]=bz/hull.w[2];
  }
  // Source-domain bilateral filter: 3x3, range-weighted, before quantization.
  // One browser-side RGB row, no frame buffer on the MCU. Do not filter IDs:
  // removing output dots would destroy the deliberate spatial color mixture.
  function sixColorRow(rgba, y, row, range) { const {width:WIDTH,height:HEIGHT}=logicalSize();
    for (let x=0;x<WIDTH;x++) {
      const center=(y*WIDTH+x)*4;
      let r=0,g=0,b=0,total=0;
      for(let dy=-1;dy<=1;dy++) {
        const yy=y+dy;if(yy<0||yy>=HEIGHT)continue;
        for(let dx=-1;dx<=1;dx++) {
          const xx=x+dx;if(xx<0||xx>=WIDTH)continue;
          const j=(yy*WIDTH+xx)*4;
          const delta=Math.max(Math.abs(rgba[j]-rgba[center]),Math.abs(rgba[j+1]-rgba[center+1]),
            Math.abs(rgba[j+2]-rgba[center+2]),Math.abs(rgba[j+3]-rgba[center+3]));
          const w=range[delta]*(dx===0?2:1)*(dy===0?2:1);
          if(!w)continue;
          const a=rgba[j+3]/255;
          r+=w*(linear[rgba[j]]*a+1-a);g+=w*(linear[rgba[j+1]]*a+1-a);b+=w*(linear[rgba[j+2]]*a+1-a);total+=w;
        }
      }
      row[x*3]=r/total;row[x*3+1]=g/total;row[x*3+2]=b/total;
    }
  }
  // Only two error rows are kept. Full-strength linear-light diffusion preserves
  // the mean ink coverage for gray, pink and other spatially mixed colors.
  const TO_SRGB = Uint8Array.from({length:4097}, (_, i) => {
    const v = i / 4096;
    return Math.round(255 * (v <= .0031308 ? 12.92*v : 1.055*v**(1/2.4)-.055));
  });
  const luma = (r,g,b) => .2126*r + .7152*g + .0722*b;
  const PRESETS = {
    photo: { levels:.55, saturation:1.13, gamma:.96, sharp:.24, protect:.8 },
    illustration: { levels:.6, saturation:1.21, gamma:1, sharp:.16, protect:.35 },
    document: { levels:.7, saturation:0, gamma:1, sharp:.55, protect:0 },
    portrait: { levels:.3, saturation:1.04, gamma:.94, sharp:.12, protect:1 },
  };

  function preprocess(input, width, height, preset) {
    const cfg = PRESETS[preset];
    if (!cfg) return input;
    const n = width * height, luminance = new Float32Array(n);
    const histogram = new Uint32Array(256);
    const output = new Uint8ClampedArray(input.length);
    for (let i = 0; i < n; i++) {
      const k = i*4, a = input[k+3]/255;
      const y = luma(linear[input[k]], linear[input[k+1]], linear[input[k+2]])*a + 1-a;
      luminance[i] = y;
      histogram[Math.floor(y*255)]++;
    }
    const percentile = target => {
      let count = 0;
      for (let i = 0; i < 256; i++) {
        count += histogram[i];
        if (count >= target*n) return i/255;
      }
      return 1;
    };
    const lo = percentile(.015), hi = percentile(.985);
    const levels = hi-lo > .2 ? cfg.levels : 0;
    for (let i = 0; i < n; i++) {
      const k = i*4, alpha = input[k+3]/255, y = luminance[i], x = i%width;
      const r=input[k], g=input[k+1], b=input[k+2];
      const skin = r > 65 && r > g && g > b && r-b > 14 ? cfg.protect : 0;
      const blur = (luminance[x ? i-1 : i] + luminance[x+1 < width ? i+1 : i] +
        luminance[i >= width ? i-width : i] + luminance[i+width < n ? i+width : i])/4;
      const level = levels ? clamp((y-lo)/(hi-lo),0,1) : y;
      const detail = clamp((y-blur)*cfg.sharp, -.12, .12) * (1-skin*.55);
      const mapped = clamp((y+(level-y)*levels)**cfg.gamma + detail, 0, 1);
      for (let c = 0; c < 3; c++) {
        const source = linear[input[k+c]]*alpha + 1-alpha;
        const sat = cfg.saturation + (1-cfg.saturation)*skin;
        output[k+c] = TO_SRGB[Math.round(clamp(mapped + (source-y)*sat, 0, 1)*4096)];
      }
      output[k+3] = 255;
    }
    return output;
  }

  function logicalSize(){return panelRotation%180?{width:720,height:480}:{width:480,height:720};} function* quantizeLegacy(rgba, options = {}) { const {width:WIDTH,height:HEIGHT}=logicalSize();
    const { gray = false, quality = 'best', style = 'photo', canceled = () => false } = options;
    if (!Object.hasOwn(MODES, quality)) throw new Error('未知混色质量');
    if (!['photo', 'illustration', 'flat'].includes(style)) throw new Error('未知呈现风格');
    if (!rgba || rgba.length !== WIDTH * HEIGHT * 4) throw new Error('裁剪图尺寸必须为 480×720');
    rgba = preprocess(rgba, WIDTH, HEIGHT, options.preset);
    const denoise = options.denoise ??'off';
    if (!['off','balanced','strong'].includes(denoise)) throw new Error('未知六色降噪等级');
    const colors = options.colors;
    const palette = Array.isArray(colors) && colors.length === 6 &&
      colors.every(color => /^#[0-9a-f]{6}$/i.test(color)) ? buildPalette(colors) : buildPalette();
    const algorithm = options.algorithm || 'floyd';
    const strength = clamp(Number(options.strength ?? 100), 0, 100) / 100;
    const cap = clamp(Number(options.errorCap ?? .55), .05, 1);
    const bayer = [0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5];
    const kernels = {
      floyd: [[1,0,7/16],[-1,1,3/16],[0,1,5/16],[1,1,1/16]],
      atkinson: [[1,0,1/8],[2,0,1/8],[-1,1,1/8],[0,1,1/8],[1,1,1/8],[0,2,1/8]],
      stucki: [[1,0,8/42],[2,0,4/42],[-2,1,2/42],[-1,1,4/42],[0,1,8/42],[1,1,4/42],[2,1,2/42],
        [-2,2,1/42],[-1,2,2/42],[0,2,4/42],[1,2,2/42],[2,2,1/42]],
    };
    const kernel = kernels[algorithm];
    const clean = style !== 'flat' && denoise !== 'off';
    const strong = denoise === 'strong';
    const filtered = clean ? new Float32Array(WIDTH*3) : null;
    const hull = clean && !gray ? sixInkHull(style, palette) : null, projected = clean ? new Float64Array(3) : null;
    const range = clean ? Float32Array.from({length:256},(_,d)=>d>(strong?48:32)?0:Math.exp(-d*d/(2*(strong?20:12)**2))) : null;
    const inks = gray ? palette.slice(0, 2) : palette;
    const mono = gray;
    const gain = style === 'flat' ? 0 : (gray ? 1 : MODES[quality]);
    const ids = new Uint8Array(WIDTH * HEIGHT);
    const preview = new Uint8ClampedArray(ids.length * 4);
    const rows = gain && kernel ? Array.from({length:3}, () => new Float32Array((WIDTH+4)*3)) : null;
    for (let y = 0; y < HEIGHT; y++) {
      if (canceled()) throw canceledError();
      if(clean) sixColorRow(rgba,y,filtered,range);
      const dir = options.serpentine !== false && (y & 1) ? -1 : 1;
      const current = rows?.[0];
      for (let step = 0; step < WIDTH; step++) {
        const x = dir === 1 ? step : WIDTH - 1 - step;
        const i = y * WIDTH + x, e = (x + 2) * 3, src = i * 4;
        const alpha = rgba[src + 3] / 255;
        let r = linear[rgba[src]] * alpha + (1 - alpha);
        let g = linear[rgba[src + 1]] * alpha + (1 - alpha);
        let b = linear[rgba[src + 2]] * alpha + (1 - alpha);
        if(clean) {r=filtered[x*3];g=filtered[x*3+1];b=filtered[x*3+2];}
        // A neutral source must not become a complementary yellow/blue pattern.
        // Gate by SOURCE color, not error-perturbed color, to avoid oscillation.
        const encode=v=>255*(v<=.0031308?12.92*v:1.055*v**(1/2.4)-.055);
        const chroma=clean ? encode(Math.max(r,g,b))-encode(Math.min(r,g,b)) : 0;
        const neutral=clean && chroma<=(strong?20:12);
        const pixelMono=mono||neutral;
        const blend=clean ? clamp((chroma-(strong?20:12))/20,0,1) : 1;
        const colorGain=blend*blend*(3-2*blend);
        const neutralBias=clean && !pixelMono ? 1-colorGain : 0;
        if(hull && !neutral) {projectSixColor(r,g,b,hull,projected);[r,g,b]=projected;}
        if (gray || neutral) r = g = b = .2126 * r + .7152 * g + .0722 * b;
        if (gain && algorithm === 'bayer') {
          const offset = (bayer[(y & 3)*4 + (x & 3)] / 16 - .47) * .27 * strength * gain;
          r += offset; g += offset; b += offset;
        }
        if (current) {
          if(pixelMono) {const v=.2126*current[e]+.7152*current[e+1]+.0722*current[e+2];r+=v;g+=v;b+=v;}
          else {r+=current[e];g+=current[e+1];b+=current[e+2];}
        }
        let best = inks[0], distance = Infinity;
        for (const ink of inks) {
          if(neutral && ink.id!==0 && ink.id!==1)continue;
          const dr = r - ink.lin[0], dg = g - ink.lin[1], db = b - ink.lin[2];
          const d = pixelMono ? (r - ink.luminance) ** 2 :
            (style === 'illustration' ? dr * dr + dg * dg + db * db :
              .3 * dr * dr + .5 * dg * dg + .2 * db * db);
          const score=d+(neutralBias && ink.id!==0 && ink.id!==1 ? .5*neutralBias*neutralBias : 0);
          if (score < distance) { best = ink; distance = score; }
        }
        ids[i] = best.id;
        preview[src] = best.rgb[0]; preview[src + 1] = best.rgb[1];
        preview[src + 2] = best.rgb[2]; preview[src + 3] = 255;
        if (rows) {
          const luminanceError=.2126*(r-best.lin[0])+.7152*(g-best.lin[1])+.0722*(b-best.lin[2]);
          for (let c = 0; c < 3; c++) {
            const value = c === 0 ? r : c === 1 ? g : b;
            let residual=value-(pixelMono ? best.luminance : best.lin[c]);
            if(clean && !pixelMono) residual=luminanceError+(residual-luminanceError)*colorGain;
            const error = clamp(residual, -cap, cap) * strength * gain;
            for (const [dx, dy, weight] of kernel) rows[dy][e + dir * dx * 3 + c] += error * weight;
          }
        }
      }
      if (rows) { const row = rows.shift(); row.fill(0); rows.push(row); }
      if ((y & 15) === 15) yield (y + 1) / HEIGHT;
    }
    return { ids, preview };
  }
  // 反射色估计值，作为校准起点。设备色号沿用 OTP 协议。
  const PANEL_COLORS = Object.freeze(['#373735','#b9b8ad','#c2a33b','#aa4436','#426896','#4b7951']);
  function toLab(r,g,b) {
    const l=Math.cbrt(.4122214708*r+.5363325363*g+.0514459929*b);
    const m=Math.cbrt(.2119034982*r+.6806995451*g+.1073969566*b);
    const s=Math.cbrt(.0883024619*r+.2817188376*g+.6299787005*b);
    return [.2104542553*l+.793617785*m-.0040720468*s,
      1.9779984951*l-2.428592205*m+.4505937099*s,.0259040371*l+.7827717662*m-.808675766*s];
  }
  function fromLab(L,a,b) {
    const l=(L+.3963377774*a+.2158037573*b)**3;
    const m=(L-.1055613458*a-.0638541728*b)**3;
    const s=(L-.0894841775*a-1.291485548*b)**3;
    return [4.0767416621*l-3.3077115913*m+.2309699292*s,
      -1.2684380046*l+2.6097574011*m-.3413193965*s,-.0041960863*l-.7034186147*m+1.707614701*s];
  }
  const TETRA = [[0,1,2,3],[0,1,2,5],[0,1,5,4],[0,1,4,3]];
  const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
  const dot=(a,b)=>a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
  const subtract=(a,b)=>[a[0]-b[0],a[1]-b[1],a[2]-b[2]];
  let lastGamut;
  function gamutFor(inks) {
    const key=inks.map(ink=>ink.css).join();
    if(lastGamut?.key===key) return lastGamut;
    const origin=inks[0].lin;
    const matrices=TETRA.map((tetra)=>{
      const [a,b,c]=tetra.slice(1).map(i=>subtract(inks[i].lin,origin));
      const rows=[cross(b,c),cross(c,a),cross(a,b)];
      const determinant=dot(a,rows[0]);
      return Math.abs(determinant)<1e-9 ? null : rows.map(row=>row.map(x=>x/determinant));
    });
    const locate=(r,g,b)=>{
      r-=origin[0];g-=origin[1];b-=origin[2];
      for(let i=0;i<4;i++) {
        const m=matrices[i]; if(!m) continue;
        const a=m[0][0]*r+m[0][1]*g+m[0][2]*b;
        const c=m[1][0]*r+m[1][1]*g+m[1][2]*b;
        const d=m[2][0]*r+m[2][1]*g+m[2][2]*b;
        if(Math.min(a,c,d)>=-1e-5 && a+c+d<=1.00001) return i;
      }
      return -1;
    };
    const table=new Int8Array(32768);
    for(let r=0;r<32;r++)for(let g=0;g<32;g++)for(let b=0;b<32;b++)
      table[(r*32+g)*32+b]=locate(r/31,g/31,b/31);
    return lastGamut={key,locate,table};
  }
  const KERNELS={
    floyd:[[1,0,7],[-1,1,3],[0,1,5],[1,1,1]],
    sierra:[[1,0,5],[2,0,3],[-2,1,2],[-1,1,4],[0,1,5],[1,1,4],[2,1,2],[-1,2,2],[0,2,3],[1,2,2]],
    stucki:[[1,0,8],[2,0,4],[-2,1,2],[-1,1,4],[0,1,8],[1,1,4],[2,1,2],[-2,2,1],[-1,2,2],[0,2,4],[1,2,2],[2,2,1]],
    jarvis:[[1,0,7],[2,0,5],[-2,1,3],[-1,1,5],[0,1,7],[1,1,5],[2,1,3],[-2,2,1],[-1,2,3],[0,2,5],[1,2,3],[2,2,1]],
  };
  // 固定种子的高通阈值纹理，压低随机纹理的低频成分。
  const threshold=(()=>{
    let seed=20261125;
    const white=Float32Array.from({length:4096},()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296-.5;});
    return white.map((v,i)=>v-.25*(white[(i&~63)|((i+1)&63)]+white[(i&~63)|((i-1)&63)]+white[(i+64)&4095]+white[(i-64)&4095]));
  })();

  function* quantizeV2(rgba,o) {
    const {width,height}=logicalSize(), size=width*height;
    if(rgba?.length!==size*4) throw new Error('裁剪图尺寸必须匹配屏幕');
    const inks=buildPalette(o.colors || PANEL_COLORS);
    const gamut=gamutFor(inks), labs=inks.map(i=>toLab(...i.lin));
    const input=new Float32Array(size*3), hist=new Uint32Array(256);
    const preset=o.engine || 'fine', quality=o.quality||'best';
    const settings=o.adjust||{};
    const hi=(settings.highlights||0)/100, sh=(settings.shadows||0)/100;
    const exp=2**((settings.exposure||0)/50);
    const brightness=(settings.brightness||0)/220;
    const contrast=1+(settings.contrast||0)/100;
    const saturation=1+(settings.saturation||0)/100;
    for(let y=0;y<height;y++){
      if(o.canceled?.()) throw canceledError();
      for(let x=0;x<width;x++){
        const i=y*width+x,k=i*4,j=i*3,alpha=rgba[k+3]/255;
        const sensor=[0,1,2].map(c=>linear[rgba[k+c]]*alpha+1-alpha);
        let l=luma(...sensor);
        const mapped=clamp((l*exp-.25)*contrast+.25+brightness+sh*.45*(1-l)**2+hi*.45*l*l,0,1);
        for(let c=0;c<3;c++) input[j+c]=clamp(mapped+(sensor[c]-l)*saturation,0,1);
        hist[Math.round(mapped*255)]++;
      }
      if((y&31)===31) yield .2*(y+1)/height;
    }
    const percentile=q=>{let sum=0;for(let i=0;i<256;i++){sum+=hist[i];if(sum>=q*size)return i/255;}return 1;};
    const low=percentile(.01),high=percentile(.99);
    const black=inks[0].luminance, white=inks[1].luminance;
    const typ=o.tone==='off'?0:o.tone==null||o.tone==='auto'?.82:clamp(Number(o.tone),0,1);
    const n=clamp(Number(o.dotGain ?? 1),1,2);
    const domain=inks.map(ink=>ink.lin.map(v=>v**(1/n)));
    const rows=Array.from({length:3},()=>new Float32Array((width+4)*3));
    const ids=new Uint8Array(size),preview=new Uint8ClampedArray(size*4);
    const taps=KERNELS[o.algorithm]||KERNELS.floyd;
    const weight=taps.reduce((sum,t)=>sum+t[2],0);
    const noise=quality==='fast'?0:Number(o.noise??(preset==='sharp'?.08:.16));
    const edge=quality==='best'?Number(o.edge??.6):0;
    const vivid=Number(o.vivid??(preset==='vivid'?1.28:1));
    const sharpen=Number(o.sharpness??(preset==='sharp'?.6:.35));
    const local=Number(o.localContrast??(preset==='vivid'?.3:.4));
    const wl=Number(o.wL??1),wc=Number(o.wC??.7);
    const at=(x,y)=>{const j=(clamp(y,0,height-1)*width+clamp(x,0,width-1))*3;return luma(input[j],input[j+1],input[j+2]);};
    for(let y=0;y<height;y++){
      if(o.canceled?.())throw canceledError();
      const dir=y&1?-1:1;
      for(let step=0;step<width;step++){
        const x=dir===1?step:width-1-step,i=y*width+x,j=i*3,e=(x+2)*3;
        const l=at(x,y), left=at(x-1,y),right=at(x+1,y),up=at(x,y-1),down=at(x,y+1);
        const base=(left+right+up+down)/4;
        const grad=Math.hypot(right-left,down-up);
        const z=high-low>.12?clamp((l-low)/(high-low),0,1):l;
        const knee=z*z*(3-2*z);
        const toned=clamp(l+typ*(black+(white-black)*(.87*z+.13*knee)-l)
          +clamp((l-base)*(sharpen*.19+local*.08),-.025,.025),0,1);
        let [L,a,b]=toLab(input[j]+toned-l,input[j+1]+toned-l,input[j+2]+toned-l);
        const hue=Math.atan2(b,a)*180/Math.PI;
        const skin=o.skin!==false&&hue>30&&hue<80&&Math.hypot(a,b)>.025&&L>.42&&L<.86;
        const sat=skin?1+(vivid-1)*.25:vivid;
        a*=sat;b*=sat;
        L=clamp(L,labs[0][0],labs[1][0]);
        let rgb=fromLab(L,a,b);
        let tetra=gamut.locate(...rgb);
        if(tetra<0&&!o.gray){
          // 沿同亮度的黑白轴压缩色度；非中性校准色板仍存在可用轴。
          const t=clamp((L-labs[0][0])/(labs[1][0]-labs[0][0]||1),0,1);
          const axis=inks[0].lin.map((v,c)=>v+t*(inks[1].lin[c]-v));
          const axisLab=toLab(...axis);
          let bottom=0,top=1, best=axis;
          for(let k=0;k<7;k++){
            const f=(bottom+top)/2;
            const probe=fromLab(axisLab[0],axisLab[1]+(a-axisLab[1])*f,axisLab[2]+(b-axisLab[2])*f);
            if(gamut.locate(...probe)>=0){bottom=f;best=probe;}else top=f;
          }
          rgb=best;tetra=gamut.locate(...rgb);
        }
        if(o.gray)rgb=[toned,toned,toned];
        const nearAxis=o.gray||Math.max(...rgb)-Math.min(...rgb)<.026;
        const value=rgb.map((v,c)=>clamp(v,0,1)**(1/n)+rows[0][e+c]);
        const query=toLab(...value.map(v=>clamp(v,0,1)**n));
        if(tetra<0){
          const u=rgb.map(v=>Math.round(clamp(v,0,1)*31));
          tetra=gamut.table[(u[0]*32+u[1])*32+u[2]];
        }
        const candidates=nearAxis?[0,1]:TETRA[tetra]||[0,1,2,3,4,5];
        query[0]+=threshold[(y&63)*64+(x&63)]*noise*.16/(1+grad*8);
        let best=candidates[0],cost=Infinity;
        for(const k of candidates){
          const q=labs[k], d=wl*(query[0]-q[0])**2+wc*((query[1]-q[1])**2+(query[2]-q[2])**2)
            +(skin&&(k===4||k===5)?.010:0);
          if(d<cost){cost=d;best=k;}
        }
        ids[i]=inks[best].id;
        preview.set(inks[best].rgb,i*4);preview[i*4+3]=255;
        const keep=1-clamp(grad/.24,0,1)*edge;
        for(let c=0;c<3;c++){
          const err=clamp(value[c]-domain[best][c],-.45,.45)*keep/weight;
          for(const [dx,dy,w] of taps)rows[dy][e+dir*dx*3+c]+=err*w;
        }
      }
      const row=rows.shift();row.fill(0);rows.push(row);
      if((y&15)===15)yield .2+.8*(y+1)/height;
    }
    return {ids,preview};
  }

  // 从稳定的经典结果出发，仅在源图平坦区抑制孤立的异色墨点。
  function* quantizeClean(rgba, options) {
    const {width,height}=logicalSize();
    const seed=quantizeLegacy(rgba,{...options,colors:options.colors || PANEL_COLORS});
    let progress=seed.next();
    while(!progress.done){yield progress.value*.85;progress=seed.next();}
    const result=progress.value;
    const inks=buildPalette(options.colors || PANEL_COLORS);
    const lookup=Array.from({length:7},(_,id)=>inks.find(ink=>ink.id===id));
    const original=result.ids.slice();
    for(let y=1;y<height-1;y++){
      if(options.canceled?.()) throw canceledError();
      for(let x=1;x<width-1;x++){
        const i=y*width+x, k=i*4, ink=original[i];
        const adjacent=[i-1,i+1,i-width,i+width];
        const replacement=original[adjacent[0]];
        if(ink<2||replacement<2||replacement===ink||!adjacent.every(j=>original[j]===replacement)) continue;
        if(!adjacent.every(j=>Math.max(Math.abs(rgba[j*4]-rgba[k]),
            Math.abs(rgba[j*4+1]-rgba[k+1]),Math.abs(rgba[j*4+2]-rgba[k+2]))<=7)) continue;
        const a=lookup[ink],b=lookup[replacement];
        const rgb=[linear[rgba[k]],linear[rgba[k+1]],linear[rgba[k+2]]];
        const error=(rgb, ref)=>.3*(rgb[0]-ref[0])**2+.5*(rgb[1]-ref[1])**2+.2*(rgb[2]-ref[2])**2;
        const mixed=b.lin.map((v,c)=>(v*4+a.lin[c])/5);
        if(error(rgb,b.lin)>error(rgb,mixed)) continue;
        result.ids[i]=replacement;
        result.preview.set(b.rgb,k);
      }
      if((y&15)===15) yield .85+.15*(y+1)/height;
    }
    return result;
  }

  function* quantizeSteps(rgba, options={}) {
    if(options.engine==='clean') return yield* quantizeClean(rgba,options);
    if(!options.engine||options.engine==='legacy')
      return yield* quantizeLegacy(rgba,options);
    return yield* quantizeV2(rgba,options);
  }

  function quantize(rgba, options) {
    const it = quantizeSteps(rgba, options);
    let step;
    do { step = it.next(); } while (!step.done);
    return step.value;
  }
  async function quantizeAsync(rgba, options) {
    if (typeof window !== 'undefined' && typeof Worker !== 'undefined') {
      try {
        return await new Promise((resolve, reject) => {
          const worker = new Worker('/eink_worker.js');
          const finish = (fn, value) => { worker.terminate(); fn(value); };
          worker.onerror = event => { event.preventDefault(); finish(reject, new Error('Worker 无法处理图片')); };
          worker.onmessage = ({data}) => {
            if (options?.canceled?.()) return finish(reject, canceledError());
            if (data.error) return finish(reject, new Error(data.error));
            if (data.progress !== undefined) options?.onProgress?.(data.progress);
            else finish(resolve, data);
          };
          const { canceled, onProgress, ...settings } = options || {};
          worker.postMessage({rgba, settings, rotation: panelRotation});
        });
      } catch (error) {
        if (error.name === 'AbortError') throw error;
      }
    }
    const it = quantizeSteps(rgba, options);
    for (;;) {
      const step = it.next();
      if (step.done) return step.value;
      options?.onProgress?.(step.value);
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }
  function checksum(bytes) {
    let hash = 0x811c9dc5;
    for (const byte of bytes) hash = Math.imul(hash ^ byte, 0x01000193) >>> 0;
    return hash.toString(16).padStart(8, '0');
  }
  function makeFrame(ids, quality = 'best') {
    if (!Object.hasOwn(MODES, quality)) throw new Error('未知混色质量');
    if (!ids || ids.length !== WIDTH * HEIGHT) throw new Error('像素数量错误');
    const body = new Uint8Array(FRAME_BYTES), counts = new Uint32Array(7);
    let offset = 0;
    for (let x = WIDTH - 1; x >= 0; x--) {
      for (let y = 0; y < HEIGHT; y += 2) {
        const a = ids[sourceIndex(x, y)], b = ids[sourceIndex(x, y + 1)];
        if (!validInk(a) || !validInk(b)) throw new Error('六色帧包含无效色号');
        body[offset++] = (a << 4) | b; counts[a]++; counts[b]++;
      }
    }
    return { rotationEpoch, body, counts, format: FORMAT, quality, pass: 0, checksum: checksum(body) };
  }
  function previewIds(ids, colors) {
    if (!(ids instanceof Uint8Array) || ids.length !== WIDTH * HEIGHT) throw new Error('像素数量错误');
    const lookup = [];
    for (const color of colors ? buildPalette(colors) : palette) lookup[color.id] = color.rgb;
    const preview = new Uint8ClampedArray(ids.length * 4);
    for (let i = 0; i < ids.length; i++) {
      const rgb = lookup[ids[i]];
      if (!rgb) throw new Error('六色预览包含无效色号');
      preview.set(rgb, i * 4); preview[i * 4 + 3] = 255;
    }
    return preview;
  }
  function composeRegion(base, next, region) { const {width:WIDTH,height:HEIGHT}=logicalSize();
    if (!(base instanceof Uint8Array) || !(next instanceof Uint8Array) || base.constructor !== next.constructor ||
        base.length !== WIDTH * HEIGHT || next.length !== base.length) throw new Error('区域更新需要完整基准画面');
    const {x, y, width, height} = region || {};
    if (![x,y,width,height].every(Number.isInteger) || x < 0 || y < 0 || width < 1 || height < 1 ||
        x + width > WIDTH || y + height > HEIGHT) throw new Error('更新区域超出屏幕');
    const result = base.slice();
    for (let row = y; row < y + height; row++) {
      const start = row * WIDTH + x;
      result.set(next.subarray(start, start + width), start);
    }
    return result;
  }
  async function verifyBaseline(baseline) {
    if (!baseline || !Number.isInteger(baseline.id) || !Number.isInteger(baseline.generation) || baseline.generation < 1) {
      throw new Error('请先完整发送一次画面，建立区域更新基准');
    }
    const job = await request('/api/job', {cache:'no-store'}, 4000);
    if (!job.ok || job.phase !== 'idle' || job.err !== 0 || job.id !== baseline.id ||
        job.display_generation !== baseline.generation || job.current_display_generation !== baseline.generation) {
      throw new Error('屏幕基准已变化，请重新发送完整画面');
    }
    return job;
  }
  // Read bodies before clearing the timer, so a stalled response also times out.
  async function request(url, options = {}, timeoutMs = 15000) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const res = await fetch(url, { ...options, signal: controller.signal });
      const body = await res.text();
      if (!res.ok) throw new Error(body && !body.trimStart().startsWith('<') ? body : `HTTP ${res.status}`);
      return JSON.parse(body);
    } catch (err) {
      if (err.name === 'AbortError') throw Object.assign(new Error('设备响应超时，请检查连接及设备日志'),
        { name: 'TimeoutError', retryable: true });
      throw err;
    } finally { clearTimeout(timer); }
  }
  async function sendFrame(frame, onPhase = () => {}, mode = 'photo', saveToAlbum = true) {
    if (!frame || frame.format !== FORMAT || frame.pass !== 0 || !(frame.body instanceof Uint8Array) ||
        frame.body.length !== FRAME_BYTES || !Object.hasOwn(MODES, frame.quality) ||
        checksum(frame.body) !== frame.checksum || (frame.baseGeneration !== undefined &&
          (!Number.isInteger(frame.baseGeneration) || frame.baseGeneration < 1))) {
      throw new Error('帧格式或校验值错误');
    }
    onPhase('正在发送六色画面，请等待屏幕完成刷新…');
    const accepted = await request('/api/frame', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-Eink-Format': frame.format,
        'X-Eink-Rotation-Epoch': String(frame.rotationEpoch), 'X-Eink-Pass': '0', 'X-Eink-Mode': mode, 'X-Eink-Storage': saveToAlbum ? 'album' : 'display', 'X-Eink-Quality': frame.quality, 'X-Eink-Checksum': frame.checksum,
        ...(frame.baseGeneration ? {'X-Eink-Base-Generation': String(frame.baseGeneration)} : {}) },
      body: frame.body,
    }, 240000);
    if (!accepted.ok || !Number.isInteger(accepted.id) || accepted.id <= 0) throw new Error('设备协议不匹配，请更新固件并刷新网页');
    if (accepted.phase === 'error' || accepted.err) throw new Error(`屏幕刷新失败 (${accepted.err})`);
    let completed = accepted;
    if (accepted.phase !== 'idle') {
      onPhase('图片已接收，屏幕刷新中…');
      completed = await waitForJob(accepted.id, 180000);
    }
    if (completed.kind !== 'frame' || completed.sequence !== FORMAT || completed.err !== 0 ||
        completed.completed_mask !== 1 || completed.plane_fnv?.length !== 1 ||
        completed.plane_fnv[0] !== frame.checksum ||
        !Number.isInteger(completed.display_generation) || completed.display_generation < 1)
      throw new Error('六色刷新完成凭据不匹配，请更新固件并刷新网页');
    return completed;
  }
  async function waitForJob(id, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const job = await request('/api/job', { cache: 'no-store' }, Math.min(15000, deadline - Date.now()));
      if (!job.ok || job.id !== id) throw new Error('刷新任务已变化，请重新发送');
      if (job.phase === 'error' || job.err) throw new Error(`屏幕刷新失败 (${job.err})`);
      if (job.phase === 'idle') return job;
      if (job.phase !== 'refresh' && job.phase !== 'receiving') throw new Error('设备返回未知刷新状态');
      await new Promise(resolve => setTimeout(resolve, 700));
    }
    throw new Error('屏幕刷新超时，请返回设备日志');
  }
  async function clearScreen() {
    const job = await request('/api/clear', {
      method: 'POST', body: '',
    });
    if (!job.ok || !Number.isInteger(job.id) || job.id <= 0 || job.kind !== 'clear-screen' ||
        job.sequence !== 'otp6-clear-v1')
      throw new Error('全局清屏请求协议不匹配');
    const completed = await waitForJob(job.id, 180000);
    if (completed.kind !== 'clear-screen' || completed.sequence !== 'otp6-clear-v1' ||
        completed.err !== 0 || completed.completed_mask !== 1 ||
        !Number.isInteger(completed.display_generation) || completed.display_generation < 1)
      throw new Error('全局清屏完成凭据不匹配');
    return completed;
  }
  return Object.freeze({ WIDTH, HEIGHT, FRAME_BYTES, FORMAT, COLORS,
    setPanelRotation, quantize, quantizeAsync, quantizeSteps, toLab, fromLab, PANEL_COLORS, checksum, makeFrame,
    previewIds, composeRegion, verifyBaseline, sendFrame, waitForJob, clearScreen });
});
