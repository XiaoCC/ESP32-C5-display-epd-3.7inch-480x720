    const { createApp, ref, onMounted, computed, nextTick, watch } = Vue;

    const Pipeline = window.EinkPipeline;
    let PANEL_W = Pipeline.WIDTH, PANEL_H = Pipeline.HEIGHT;
    const COLORS = Pipeline.COLORS;
    const cssOf = (id) => COLORS.find(c => c.id === id).css;
    const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

    const fetchJson = async (url, options = {}, timeoutMs = 15000) => {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), timeoutMs);
      try {
        const res = await fetch(url, { ...options, signal: controller.signal });
        const body = await res.text();
        let info;
        try { info = JSON.parse(body); }
        catch (_) { throw new Error(res.ok ? '设备返回了无效数据' : (body.trim().slice(0, 100) || `设备请求失败（${res.status}）`)); }
        if (!res.ok || info?.ok === false) throw new Error(info?.message || `设备请求失败（${res.status}）`);
        return info;
      } finally { clearTimeout(timer); }
    };

    const applyAdjust = (data, { exposure, brightness, contrast, highlights, shadows, saturation }) => {
      const exp = Math.pow(2, exposure / 50);
      const bri = brightness * 1.2;
      const con = (contrast + 100) / 100;
      const sat = (saturation + 100) / 100;
      const hi = highlights / 100;
      const sh = shadows / 100;
      for (let i = 0; i < data.length; i += 4) {
        let r = data[i], g = data[i + 1], b = data[i + 2];
        r *= exp; g *= exp; b *= exp;
        r = (r - 128) * con + 128 + bri;
        g = (g - 128) * con + 128 + bri;
        b = (b - 128) * con + 128 + bri;
        let y = 0.299 * r + 0.587 * g + 0.114 * b;
        if (y > 128 && hi !== 0) {
          const t = (y - 128) / 127;
          const k = 1 - hi * t * 0.85;
          r = 128 + (r - 128) * k;
          g = 128 + (g - 128) * k;
          b = 128 + (b - 128) * k;
          y = 0.299 * r + 0.587 * g + 0.114 * b;
        }
        if (y < 128 && sh !== 0) {
          const t = (128 - y) / 128;
          const lift = sh * t * 55;
          r += lift; g += lift; b += lift;
          y = 0.299 * r + 0.587 * g + 0.114 * b;
        }
        r = y + (r - y) * sat;
        g = y + (g - y) * sat;
        b = y + (b - y) * sat;
        data[i] = clamp(r, 0, 255);
        data[i + 1] = clamp(g, 0, 255);
        data[i + 2] = clamp(b, 0, 255);
      }
    };

    createApp({
      setup() {
        const tab = ref('wifi');
        const wifiMode = ref('ap');
        const wifiSsid = ref('');
        const wifiUrl = ref('http://192.168.4.1');
        const wifiList = ref([]);
        const selectedSsid = ref('');
        const wifiPass = ref('');
        const wifiBusy = ref(false);
        const wifiStatus = ref('');
        const wifiStatusClass = ref('');
        const chargePower = ref('balanced');
        const chargeCapacity = ref(500);
        const chargeDirty = ref(false);
        const chargeReady = ref(false);
        const chargeDetail = ref('正在读取充电配置');
        const gaugeDetail = ref('');
        const applyChargeInfo = (info, saved = false) => {
          chargePower.value = info.charge;
          if (!chargeDirty.value || saved) chargeCapacity.value = info.capacity_mah;
          if (saved) chargeDirty.value = false;
          chargeReady.value = true;
          chargeDetail.value = info.capacity_mah + 'mAh：目标 ' + info.target_ma + 'mA，恒流设定 ' + info.icc_ma + 'mA（非实测）';
          gaugeDetail.value = !info.gauge_read_ok ? '电量计读取失败' : !info.battery_present ? '未检测到电池' :
            '电量计' + (info.gauge_enabled ? '已启用' : '未启用') + '，SOC ' + (info.soc >= 0 ? info.soc + '%' : '无效') + '；内部自动学习，无可读完成度。';
        };
        const chargeBusy = ref(false);
        const chargeStatus = ref('');
        const chargeStatusClass = ref('');
        const chargeLevels = [
          { id: 'slow', label: '涓流充电', note: '0.2C' },
          { id: 'balanced', label: '正常充电', note: '0.5C · 默认档' },
          { id: 'fast', label: '快速充电', note: '1.0C' },
        ];
        const text = ref('你好，Eink');
        const bg = ref(0x1);
        const fg = ref(0x0);
        const fontSize = ref(42);
        const busy = ref(false); const panelWidth=ref(PANEL_W), panelHeight=ref(PANEL_H), panelRotation=ref(0);
        const status = ref('');
        const phase = ref('idle');
        const deviceOnline = ref(false);
        const wifiPowerBusy = ref(false);
        const wifiOffRequested = ref(false);
        let wifiOffAt = 0;
        const turnOffWifi = async () => {
          wifiPowerBusy.value = true;
          try {
            await fetchJson('/api/wifi/off', { method: 'POST' }, 5000);
            wifiOffAt = Date.now();
            wifiOffRequested.value = true;
            deviceOnline.value = false;
            wifiStatus.value = '';
          } catch (_) {
            wifiStatusClass.value = 'err';
            wifiStatus.value = '未能关闭 Wi-Fi，可长按设备 BOOT 关闭。';
          } finally { wifiPowerBusy.value = false; }
        };
        const batteryLevel = ref(null);
        const lowBattery = ref(false);
        const internetConfirmed = ref(false);
        const quant = ref('color');
        const lastPanelFrame = ref(null), regionUpdate = ref(false), forceRefresh = ref(false);
        const updateRect = ref({x:0, y:0, width:PANEL_W, height:PANEL_H});
        const canRegionUpdate = computed(() => !!lastPanelFrame.value);
        const ditherQuality = ref('best');
        const ditherModes = [
          { id: 'fast', name: '轻度', note: '轻度扩散' },
          { id: 'standard', name: '标准', note: '标准扩散' },
          { id: 'best', name: '精细', note: '完整扩散 · 保留层次' },
        ];
        const previewPixels=ref(false), noisePreviews=ref({}), noisePreviewBusy=ref(false), noiseCompareOpen=ref(false);
        const sixDenoise = ref('balanced');
        const inkColors = ref([...Pipeline.PANEL_COLORS]);
        const inkAlgorithm = ref('floyd');
        const inkEngine = ref('legacy');
        const advancedInk = ref({tone:.82, vivid:null, wL:1, wC:.7, noise:null, edge:.6, dotGain:1,
          sharpness:null, localContrast:null, skin:true});
        const adjustments = () => ({exposure:exposure.value, brightness:brightness.value, contrast:contrast.value,
          highlights:highlights.value, shadows:shadows.value, saturation:saturation.value});
        const restorePalette = () => { inkColors.value = [...Pipeline.PANEL_COLORS]; scheduleDither(); return savePalette(); };
        const photoPreset = ref('photo');
        const showOriginal = ref(false);
        const inkStrength = ref(100);
        const inkCap = ref(.55);
        const inkSerpentine = ref(true);
        const conversionProgress = ref(0);
        const paletteMessage = ref('');
        const inkOptions = () => ({...Object.fromEntries(Object.entries(advancedInk.value).filter(([,v])=>v!==null)),
          engine:inkEngine.value, adjust:inkEngine.value==='legacy'?undefined:adjustments(), colors:[...inkColors.value], algorithm:inkAlgorithm.value,
          strength:inkStrength.value, errorCap:inkCap.value, serpentine:inkSerpentine.value, preset:photoPreset.value});
        const calibrationBusy = ref(false);
        const calibrationPatch = async ink => {
          if (calibrationBusy.value) return;
          calibrationBusy.value = true;
          paletteMessage.value = '屏幕正在刷新';
          try {
            const job = await fetchJson('/api/calibrate/patch?ink='+ink, {method:'POST'},5000);
            await Pipeline.waitForJob(job.id,180000);
            paletteMessage.value = '屏幕已更新';
          } catch (e) { paletteMessage.value = '校准刷新失败：'+e.message; }
          finally { calibrationBusy.value = false; }
        };
        const calibrationPhoto = ref(null);
        const sampleTarget = ref(-1);
        let whiteCard = null;
        let samples = Array.from({length:6},()=>[]);
        const openCalibrationPhoto = async event => {
          const file=event.target.files?.[0], canvas=calibrationPhoto.value;
          if(!file || !canvas) return;
          if(file.size>24*1024*1024){paletteMessage.value='请选择不超过 24 MiB 的照片';return;}
          const url=URL.createObjectURL(file);
          try {
            const image=new Image();image.src=url;await image.decode();
            const scale=Math.min(1,960/Math.max(image.width,image.height));
            canvas.width=Math.max(1,Math.round(image.width*scale));
            canvas.height=Math.max(1,Math.round(image.height*scale));
            canvas.getContext('2d',{willReadFrequently:true}).drawImage(image,0,0,canvas.width,canvas.height);
            whiteCard=null;samples=Array.from({length:6},()=>[]);
            paletteMessage.value='照片已载入，先在白卡上点击取样。';
          } catch(e){paletteMessage.value='照片无法解码';}
          finally {URL.revokeObjectURL(url);}
        };
        const sampleCalibration = event => {
          const canvas=event.currentTarget, rect=canvas.getBoundingClientRect();
          if(canvas.width<7||canvas.height<7) return;
          const x=clamp(Math.floor((event.clientX-rect.left)/rect.width*canvas.width)-3,0,canvas.width-7);
          const y=clamp(Math.floor((event.clientY-rect.top)/rect.height*canvas.height)-3,0,canvas.height-7);
          const pixels=canvas.getContext('2d').getImageData(x,y,7,7).data;
          const channel=[0,0,0];
          const linear=v=>v<=10?v/3294.6:((v/255+.055)/1.055)**2.4;
          for(let i=0;i<pixels.length;i+=4)for(let c=0;c<3;c++)channel[c]+=linear(pixels[i+c])/49;
          if(sampleTarget.value<0){whiteCard=channel;paletteMessage.value='已采白卡，选择一个墨色继续采样';return;}
          if(!whiteCard||whiteCard.some(v=>v<.08)){paletteMessage.value='请先采样亮度足够的白卡';return;}
          const n=sampleTarget.value;
          samples[n].push(channel.map((v,c)=>v/whiteCard[c]));
          const rgb=channel.map((_,c)=>{
            const l=clamp(samples[n].reduce((sum,r)=>sum+r[c],0)/samples[n].length,0,1);
            return Math.round(255*(l<=.0031308?12.92*l:1.055*l**(1/2.4)-.055));
          });
          inkColors.value[n]='#'+rgb.map(v=>v.toString(16).padStart(2,'0')).join('');
          paletteMessage.value='已采样 '+samples[n].length+' 次，保存后生效';
        };
        const loadPalette = async () => {
          try {
            const colors = await fetchJson('/api/palette', {cache:'no-store'}, 5000);
            if (Array.isArray(colors) && colors.length === 6) inkColors.value = colors;
          } catch (_) { paletteMessage.value = '暂时无法读取设备色板'; }
        };
        const savePalette = async () => {
          try {
            await fetchJson('/api/palette', {method:'POST', body:JSON.stringify(inkColors.value)}, 5000);
            paletteMessage.value = '已将色板保存到设备';
          } catch (_) { paletteMessage.value = '色板保存失败，请检查设备连接'; }
        };

        const syncStatus = ref({});
        const statusDetails = ref({});
        const syncMessage = ref('');
        const syncActionBusy = ref(false);
        const syncStart = ref('03:30');
        const syncEnd = ref('05:30');
        const formatSyncTime = value => value > 0
          ? new Date(value * 1000).toLocaleString('zh-CN', {timeZone:'Asia/Shanghai'}) : '—';
        const formatMinute = value =>
          `${String(Math.floor(value / 60)).padStart(2, '0')}:${String(value % 60).padStart(2, '0')}`;
        const syncLabel = mask => [[1,'天气'],[2,'历史'],[4,'行程'],[8,'抵达日天气'],[16,'手动'],[32,'校时']]
          .filter(([bit]) => mask & bit).map(([, label]) => label).join('、');
        const loadSync = async (readWindow = false) => {
          try {
            const data = await fetchJson('/api/sync', {cache:'no-store'}, 5000);
            syncStatus.value = data;
            if (readWindow) {
              syncStart.value = formatMinute(data.start);
              syncEnd.value = formatMinute(data.end);
            }
          } catch (_) { syncStatus.value = {}; }
        };
        const postSync = async (fields, success) => {
          if (syncActionBusy.value) return;
          syncActionBusy.value = true;
          try {
            await fetchJson('/api/sync', {method:'POST', body:JSON.stringify(fields)}, 5000);
            syncMessage.value = success;
            await loadSync();
          } catch (_) { syncMessage.value = '同步操作失败，请检查设备连接及时间设置'; }
          finally { syncActionBusy.value = false; }
        };
        const syncNow = () => postSync({now:true}, '同步请求已提交');
        const saveSyncWindow = () => {
          const parseMinute = text => /^\d{2}:\d{2}$/.test(text)
            ? Number(text.slice(0,2))*60 + Number(text.slice(3)) : -1;
          const start = parseMinute(syncStart.value), end = parseMinute(syncEnd.value);
          if (start < 0 || end < 0 || start === end) {
            syncMessage.value = '请输入不同的起止时间';
            return;
          }
          return postSync({start,end}, '同步窗口已保存');
        };

        const sixDenoiseModes = [
          {id:'off',name:'关闭',note:'保留原始六色抖动'},
          {id:'balanced',name:'标准降噪',note:'保护灰色 · 平滑细碎杂点'},
          {id:'strong',name:'较强降噪',note:'减少暗部杂色 · 弱色彩会变淡'},
        ];
        const renderStyle = ref('illustration');
        const renderStyles = [
          { id: 'photo', name: '柔和照片', note: '平滑明暗' },
          { id: 'illustration', name: '清晰插画', note: '保留色彩细节' },
          { id: 'flat', name: '纯色图形', note: '适合图标与文字 · 六色直取' },
        ];
        const imageName = ref('');
        const imageEl = ref(null);
        const imageRotation = ref(0);
        const imgW = ref(0);
        const imgH = ref(0);
        const canvas = ref(null), appearanceCanvas = ref(null);
        const fileInput = ref(null);
        let imageSource = null;
        const colors = COLORS;
        const previewBusy = ref(false);
        const usedColors = ref(0);
        const draggingFile = ref(false);

        const zoom = ref(1);
        const panX = ref(0.5);
        const panY = ref(0.5);
        const exposure = ref(0);
        const brightness = ref(0);
        const contrast = ref(0);
        const highlights = ref(0);
        const shadows = ref(0);
        const saturation = ref(0);

        const dragging = ref(false);
        let panStart = null;
        let ditherTimer = null;
        let cropAnimation = null;
        let previewRevision = 0;
        let imageRevision = 0;

        /* 配色只记在浏览器里，不写设备也不进任何接口。 */
        const themes = [
          { id: 'system', name: '跟随系统', bg: '#e7e7e4', accent: '#626e7a' },
          { id: 'paper', name: '纸面', bg: '#e7e7e4', accent: '#1f4b78' },
          { id: 'mono', name: '黑白', bg: '#ffffff', accent: '#111111' },
          { id: 'graphite', name: '石墨', bg: '#17181a', accent: '#8fb8d8' },
          { id: 'amber', name: '琥珀', bg: '#16130d', accent: '#e8a33d' },
          { id: 'blueprint', name: '蓝图', bg: '#0d1622', accent: '#4ea3e0' },
        ];
        const theme = ref('paper');
        const themeName = computed(() => (themes.find(t => t.id === theme.value) || themes[0]).name);
        const applyTheme = (id) => {
          const actual = id === 'system' ? (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'graphite' : 'paper') : id;
          const item = themes.find(t => t.id === actual) || themes[1];
          const root = document.documentElement;
          if (root && root.setAttribute) root.setAttribute('data-theme', item.id);
          const meta = document.querySelector && document.querySelector('meta[name="theme-color"]');
          if (meta && meta.setAttribute) meta.setAttribute('content', item.bg);
        };
        const setTheme = (id) => {
          if (!themes.some(t => t.id === id)) return;
          theme.value = id;
          applyTheme(id);
          try { localStorage.setItem('eink.ui.theme.v1', id); } catch (_) {}
        };
        const restoreTheme = () => {
          let saved = '';
          try { saved = localStorage.getItem('eink.ui.theme.v1') || ''; } catch (_) {}
          if (!themes.some(t => t.id === saved)) saved = 'system';
          setTheme(saved);
          window.matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', () => {
            if (theme.value === 'system') applyTheme('system');
          });
        };

        const statusClass = computed(() => {
          if (phase.value === 'error') return 'err';
          if (phase.value === 'done') return 'ok';
          return '';
        });
        const busyLabel = computed(() => {
          if (!busy.value) return regionUpdate.value ? '发送区域替换画面' : '发送到屏幕';
          return phase.value === 'prepare' ? '生成点阵' : '屏幕刷新中';
        });

        const wrapLines = (ctx, content, maxWidth) => {
          const lines = [];
          for (const para of String(content || '').split(/\n/)) {
            if (!para) { lines.push(''); continue; }
            let line = '';
            for (const ch of para) {
              const trial = line + ch;
              if (ctx.measureText(trial).width > maxWidth && line) {
                lines.push(line);
                line = ch;
              } else line = trial;
            }
            lines.push(line);
          }
          return lines;
        };

        const rotatedSize = (img) => {
          const w = img.naturalWidth || img.width, h = img.naturalHeight || img.height;
          return imageRotation.value % 180 ? { w: h, h: w } : { w, h };
        };
        const rotateImage = (delta) => {
          if (busy.value || !imageEl.value) return;
          imageRotation.value = (imageRotation.value + delta + 360) % 360;
          resetCrop();
        };
        const cropRect = () => {
          const img = imageEl.value;
          if (!img) return null;
          const { w: iw, h: ih } = rotatedSize(img);
          const targetAspect = PANEL_W / PANEL_H;
          let baseW, baseH;
          if (iw / ih > targetAspect) {
            baseH = ih;
            baseW = ih * targetAspect;
          } else {
            baseW = iw;
            baseH = iw / targetAspect;
          }
          const sw = baseW / zoom.value;
          const sh = baseH / zoom.value;
          const maxX = Math.max(0, iw - sw);
          const maxY = Math.max(0, ih - sh);
          return {
            sx: panX.value * maxX,
            sy: panY.value * maxY,
            sw,
            sh,
          };
        };

        const renderText = () => {
          const el = canvas.value;
          if (!el) return;
          const ctx = el.getContext('2d', { willReadFrequently: true });
          ctx.fillStyle = cssOf(bg.value);
          ctx.fillRect(0, 0, PANEL_W, PANEL_H);
          ctx.fillStyle = cssOf(fg.value);
          ctx.textAlign = 'center';
          ctx.textBaseline = 'middle';
          ctx.font = `600 ${fontSize.value}px "Songti SC","Noto Serif SC","PingFang SC","Microsoft YaHei",serif`;
          const lines = wrapLines(ctx, text.value, PANEL_W - 56);
          const lineH = fontSize.value * 1.25;
          let y = (PANEL_H - lines.length * lineH) / 2 + lineH / 2;
          for (const line of lines) {
            ctx.fillText(line, PANEL_W / 2, y);
            y += lineH;
          }
        };

        /* 拖动时实时显示原图取景；松手后生成同一裁剪范围的屏幕配色。 */
        const scratch = document.createElement('canvas');
        scratch.width = PANEL_W;
        scratch.height = PANEL_H;

        const grabCrop = (drawOnly = false) => {
          const img = imageEl.value;
          const rect = cropRect();
          if (!img || !rect) return null;
          const ctx = scratch.getContext('2d', { willReadFrequently: true });
          ctx.fillStyle = '#ffffff';
          ctx.fillRect(0, 0, PANEL_W, PANEL_H);
          ctx.imageSmoothingEnabled = true;
          ctx.imageSmoothingQuality = 'high';
          if (imageRotation.value === 0) {
            ctx.drawImage(img, rect.sx, rect.sy, rect.sw, rect.sh, 0, 0, PANEL_W, PANEL_H);
          } else {
              ctx.save();
              ctx.scale(PANEL_W / rect.sw, PANEL_H / rect.sh);
              ctx.translate(-rect.sx, -rect.sy);
              const size = rotatedSize(img);
              ctx.translate(size.w / 2, size.h / 2);
              ctx.rotate(imageRotation.value * Math.PI / 180);
              ctx.drawImage(img, -(img.naturalWidth || img.width) / 2, -(img.naturalHeight || img.height) / 2);
              ctx.restore();
          }
          if (drawOnly) return scratch;
          const imgData = ctx.getImageData(0, 0, PANEL_W, PANEL_H);
          if(inkEngine.value==='legacy') applyAdjust(imgData.data, {
            exposure: exposure.value,
            brightness: brightness.value,
            contrast: contrast.value,
            highlights: highlights.value,
            shadows: shadows.value,
            saturation: saturation.value,
          });
          return imgData;
        };

        const paintPanelPreview = (ids, preview) => {
          const el = canvas.value;
          if (!el) return;
          usedColors.value = new Set(ids).size;
          el.getContext('2d').putImageData(new ImageData(preview, PANEL_W, PANEL_H), 0, 0);
          if(appearanceCanvas.value){const ctx=appearanceCanvas.value.getContext('2d');ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';ctx.drawImage(el,0,0,240,360);}
        };

        // Generate all cards from the same crop/options; never replace transmitted IDs.
        const refreshNoisePreviews = async (data, current, revision, options) => {
          if(!noiseCompareOpen.value || options.style==='flat')return;
          const full=document.createElement('canvas'),small=document.createElement('canvas');
          if(typeof small.toDataURL!=='function')return;
          full.width=PANEL_W;full.height=PANEL_H;small.width=144;small.height=216;
          noisePreviewBusy.value=true;
          const previews={};
          try {
            for(const mode of sixDenoiseModes) {
              if(revision!==previewRevision || !noiseCompareOpen.value)return;
              const result=mode.id===options.denoise?current:await Pipeline.quantizeAsync(data,{...options,denoise:mode.id,canceled:()=>revision!==previewRevision || !noiseCompareOpen.value});
              if(revision!==previewRevision || !noiseCompareOpen.value)return;
              full.getContext('2d').putImageData(new ImageData(result.preview,PANEL_W,PANEL_H),0,0);
              const ctx=small.getContext('2d');ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';
              ctx.drawImage(full,0,0,144,216);previews[mode.id]=small.toDataURL('image/png');
              noisePreviews.value={...previews};
            }
          } catch(e) {if(e.name!=='AbortError')console.warn('降噪对照预览未完成',e);}
          finally {if(revision===previewRevision)noisePreviewBusy.value=false;}
        };

        const renderImageDithered = async () => {
          const revision = ++previewRevision;
          previewBusy.value = true;
          conversionProgress.value = 0;
          try {
            const imgData = grabCrop();
            if (!imgData) throw new Error('没有可处理的图像');
            const options = {
              gray: quant.value === 'gray', quality: ditherQuality.value, style: renderStyle.value,
              denoise: sixDenoise.value, ...inkOptions(),
              onProgress: value => { if (revision === previewRevision) conversionProgress.value = Math.round(value * 100); },
              canceled: () => revision !== previewRevision,
            };
            const result = await Pipeline.quantizeAsync(imgData.data, options);
            if (revision !== previewRevision) return null;
            if (regionUpdate.value) {
              if (!canRegionUpdate.value) throw new Error('请先完整发送一次画面，再使用区域替换');
              const ids = Pipeline.composeRegion(lastPanelFrame.value.ids, result.ids, updateRect.value);
              paintPanelPreview(ids, Pipeline.previewIds(ids, inkColors.value));
              return ids;
            }
            if (showOriginal.value) {
              const original = grabCrop(true);
              if (original) canvas.value.getContext('2d').drawImage(original,0,0);
            } else paintPanelPreview(result.ids, result.preview);
            if(!busy.value)void refreshNoisePreviews(imgData.data,result,revision,options);
            return result.ids;
          } catch (e) {
            if (e.name === 'AbortError') return null;
              throw e;
          } finally {
            if (revision === previewRevision) previewBusy.value = false;
          }
        };

        const paintLiveCrop = () => {
          if(cropAnimation !== null)return;
          cropAnimation=requestAnimationFrame(() => {
            cropAnimation=null;
            if(!dragging.value || busy.value || tab.value!=='image' || !imageEl.value || !canvas.value)return;
            const crop=grabCrop(true);
            if(crop)canvas.value.getContext('2d').drawImage(crop,0,0);
          });
        };

        const scheduleDither = () => {
          if (busy.value) return;
          ++previewRevision;
          noisePreviews.value={};noisePreviewBusy.value=false;
          clearTimeout(ditherTimer);
          previewBusy.value = !!imageEl.value;
          if (!imageEl.value) return;
          if(dragging.value) {paintLiveCrop();return;}
          ditherTimer = setTimeout(() => {
            renderImageDithered().catch(e => {
              phase.value = 'error';
              status.value = '预览失败：' + e.message;
            });
          }, 90);
        };

        const onCropChange = () => {
          if(regionUpdate.value) {
            const rect=updateRect.value;
            const number=(value,fallback)=>Number.isFinite(Number(value))?Math.floor(Number(value)):fallback;
            const x=clamp(number(rect.x,0),0,PANEL_W-1),y=clamp(number(rect.y,0),0,PANEL_H-1);
            updateRect.value={x,y,width:clamp(number(rect.width,1),1,PANEL_W-x),height:clamp(number(rect.height,1),1,PANEL_H-y)};
          }
          scheduleDither();
        };
        const onAdjustChange = () => scheduleDither();

        const setRegionUpdate = enabled => {
          if (busy.value || enabled && !canRegionUpdate.value) return;
          regionUpdate.value = enabled;
          scheduleDither();
        };

        const setQuant = (q) => {
          if (busy.value || !['gray','color'].includes(q)) return;
          quant.value = q;
          if (imageEl.value) scheduleDither();
        };

        const setDitherQuality = (mode) => {
          if (busy.value || !ditherModes.some(m=>m.id===mode)) return;
          ditherQuality.value = mode;
          if (imageEl.value) scheduleDither();
        };

        const onNoiseCompareToggle = event => {
          noiseCompareOpen.value=event.target.open;
          if(noiseCompareOpen.value && imageEl.value)scheduleDither();
          else {noisePreviews.value={};noisePreviewBusy.value=false;}
        };

        const setSixDenoise = (mode) => {
          if(busy.value || !sixDenoiseModes.some(m=>m.id===mode))return;
          sixDenoise.value=mode;
          if(imageEl.value)scheduleDither();
        };

        const setRenderStyle = (style) => {
          if (busy.value || !renderStyles.some(s=>s.id===style)) return;
          renderStyle.value = style;
          if (imageEl.value) scheduleDither();
        };

        const resetCrop = () => {
          zoom.value = 1;
          panX.value = 0.5;
          panY.value = 0.5;
          scheduleDither();
        };

        const resetAdjust = () => {
          exposure.value = 0;
          brightness.value = 0;
          contrast.value = 0;
          highlights.value = 0;
          shadows.value = 0;
          saturation.value = 0;
          scheduleDither();
        };

        const switchTab = (t) => {
          if (busy.value) return;
          regionUpdate.value = false;
          ++previewRevision;
          clearTimeout(ditherTimer);
          previewBusy.value = false;
          tab.value = t;
          if (t !== 'image') { ++albumEditRevision; ++imageRevision; }
          if (t === 'image') loadAlbum(0);
          if (t === 'weather' && !weatherBusy.value) loadWeatherLocation();
          if (t === 'weather' && !weatherPolicyBusy.value) loadWeatherPolicy();
          if (t === 'todo' && !todoLoaded.value && !todoBusy.value) loadTodo();
          if (t === 'quote' && !quotesLoaded.value && !quoteBusy.value) loadQuotes();
          if (t === 'trip' && !tripsLoaded.value && !tripBusy.value) loadTrips();
          if (t === 'history' && !historyBusy.value) loadHistory();
          if (t === 'settings' && !scheduleBusy.value && !scheduleDirty.value) loadDisplaySchedule();
          if (t === 'settings' && !monoBusy.value) loadDisplayStyle();
          if (t === 'settings' && !shortcutBusy.value) loadShortcut();
          nextTick(() => {
            if (t === 'text') renderText();
            else if (t === 'image' && imageEl.value) scheduleDither();
            else {
              const el = canvas.value;
              if (!el) return;
              const ctx = el.getContext('2d');
              ctx.fillStyle = cssOf(1);
              ctx.fillRect(0, 0, PANEL_W, PANEL_H);
            }
          });
        };

        const onPanStart = (ev) => {
          if (busy.value || tab.value !== 'image' || !imageEl.value) return;
          dragging.value = true;
          panStart = {
            x: ev.clientX,
            y: ev.clientY,
            panX: panX.value,
            panY: panY.value,
          };
          ev.currentTarget.setPointerCapture?.(ev.pointerId);
          scheduleDither();
        };

        const onPanMove = (ev) => {
          if (busy.value || !dragging.value || !panStart || !imageEl.value) return;
          const el = ev.currentTarget;
          const rect = el.getBoundingClientRect();
          const img = imageEl.value;
          const crop = cropRect();
          if (!crop) return;
          const maxX = Math.max(1, rotatedSize(img).w - crop.sw);
          const maxY = Math.max(1, rotatedSize(img).h - crop.sh);
          /* 拖动方向与取景相反：手指右移 → 画面右移（看左边内容） */
          const dx = (ev.clientX - panStart.x) / rect.width;
          const dy = (ev.clientY - panStart.y) / rect.height;
          panX.value = clamp(panStart.panX - dx * (crop.sw / maxX) * zoom.value, 0, 1);
          panY.value = clamp(panStart.panY - dy * (crop.sh / maxY) * zoom.value, 0, 1);
          scheduleDither();
        };

        const onPanEnd = () => {
          if(!dragging.value)return;
          dragging.value = false;
          panStart = null;
          if(cropAnimation !== null) {cancelAnimationFrame(cropAnimation);cropAnimation=null;}
          scheduleDither();
        };

        const onWheel = (ev) => {
          if (busy.value || tab.value !== 'image' || !imageEl.value) return;
          ev.preventDefault();
          const next = clamp(zoom.value + (ev.deltaY > 0 ? -0.08 : 0.08), 1, 4);
          zoom.value = Math.round(next * 100) / 100;
          scheduleDither();
        };

        const push = async () => {
          if (busy.value) return;
          busy.value = true;
          phase.value = 'prepare';
          status.value = '生成点阵…';
          clearTimeout(ditherTimer);
          ++previewRevision;
          ++imageRevision;
          const quality = ditherQuality.value;
          try {
            await nextTick();
            let ids;
            if (tab.value === 'image') {
              ids = await renderImageDithered();
              if (!ids) throw new Error('图像已变化，重新发送');
            } else {
              renderText();
              const ctx = canvas.value.getContext('2d', { willReadFrequently: true });
              const result = await Pipeline.quantizeAsync(ctx.getImageData(0, 0, PANEL_W, PANEL_H).data,
                { quality, style: 'flat' });
              ids = result.ids;
              paintPanelPreview(ids, result.preview);
            }
            const frame = Pipeline.makeFrame(ids, quality);
            const baseline = lastPanelFrame.value;
            if (tab.value === 'image' && regionUpdate.value) {
              await Pipeline.verifyBaseline(baseline);
              frame.baseGeneration = baseline.generation;
            }
            if (!forceRefresh.value && baseline?.mode === tab.value &&
                ids.every((id,i) => id === baseline.ids[i])) {
              try {
                await Pipeline.verifyBaseline(baseline);
                phase.value = 'done'; status.value = '画面无变化'; return;
              } catch (e) {
                lastPanelFrame.value = null;
                if (regionUpdate.value) throw e;
              }
            }
            phase.value = 'refresh';
            const report = await Pipeline.sendFrame(frame, message => { status.value = message; },
              tab.value === 'text' ? 'text' : 'photo', false);
            lastPanelFrame.value = Number.isInteger(report?.display_generation) && report.display_generation > 0
              ? {id:report.id, generation:report.display_generation, ids, mode:tab.value} : null;
            phase.value = 'done';
            status.value = '已刷新到屏幕';
          } catch (e) {
            lastPanelFrame.value = null;
            regionUpdate.value = false;
            phase.value = 'error';
            status.value = '失败：' + (e.message || e);
          } finally {
            busy.value = false;
          }
        };

        const clearDisplay = async () => {
          if (busy.value) return;
          busy.value = true;
          lastPanelFrame.value = null;
          regionUpdate.value = false;
          phase.value = 'refresh';
          status.value = '正在清空屏幕…';
          try {
            const report = await Pipeline.clearScreen();
            phase.value = 'done';
            status.value = `屏幕已清空 · ${report.total_ms || 0} ms`;
          } catch (e) {
            phase.value = 'error';
            status.value = '清屏失败：' + (e.message || e);
          } finally {
            busy.value = false;
          }
        };

        const loadFile = async (file, preserveOrientation = false) => {
          if (busy.value || !file) return;
          if (!preserveOrientation && (!file.type || !file.type.startsWith('image/'))) {
            phase.value = 'error';
            status.value = '失败：不是图片文件';
            return;
          }
          status.value = '正在载入图片…';
          phase.value = 'idle';
          const revision = ++imageRevision;
          let url;
          try {
            url = URL.createObjectURL(file);
            const img = await new Promise((resolve, reject) => {
              const im = new Image();
              im.onload = () => resolve(im);
              im.onerror = () => reject(new Error('图片无法解码'));
              im.src = url;
            });
            if (revision !== imageRevision || busy.value) return;
            imageEl.value = img;
            imageSource = preserveOrientation && file.name.endsWith(".E6P.png") ? null : file;
            regionUpdate.value = false;
            status.value = '已载入，调整取景后即可发送';
            imageRotation.value = !preserveOrientation && img.naturalWidth > img.naturalHeight ? 90 : 0;
            imageName.value = file.name;
            imgW.value = img.naturalWidth || img.width;
            imgH.value = img.naturalHeight || img.height;
            tab.value = 'image';
            resetCrop();
            resetAdjust();
            await nextTick();
            scheduleDither();
          } catch (e) {
            if (revision !== imageRevision) return;
            phase.value = 'error';
            status.value = '失败：' + (e.message || e);
          } finally {
            if (url) URL.revokeObjectURL(url);
          }
        };

        const selectPhotoFiles = async files => {
          if (busy.value || !files.length) return;
          const invalid = files.find(file => file.size > 64 * 1024 * 1024 || !file.size || !file.type.startsWith('image/'));
          if (invalid) {
            albumStatus.value = status.value = `无法载入 ${invalid.name}，请选择小于或等于 64 MiB 的图片。`;
            phase.value = 'error';
            return;
          }
          ++albumEditRevision;
          albumSelection.value = [];
          albumFiles.value = files;
          albumStatus.value = '';
          if (files.length === 1) {
            await loadFile(files[0]);
          } else {
            ++imageRevision;
            imageEl.value = null;
            imageName.value = '';
            regionUpdate.value = false;
            status.value = '';
          }
        };

        const onFileChange = async ev => {
          const files = Array.from(ev.target.files || []);
          ev.target.value = '';
          await selectPhotoFiles(files);
        };

        const onDrop = async ev => {
          draggingFile.value = false;
          await selectPhotoFiles(Array.from(ev.dataTransfer?.files || []));
        };

        const albumFiles = ref([]);
        const albumPhotos = ref([]);
        let draggedAlbumId = '';
        const beginAlbumDrag = (event, id) => {
          draggedAlbumId = id;
          event.dataTransfer.effectAllowed = 'move';
          event.dataTransfer.setData('text/plain', id);
        };
        const dropAlbumPhoto = id => {
          const from = albumPhotos.value.findIndex(photo => photo.id === draggedAlbumId);
          const to = albumPhotos.value.findIndex(photo => photo.id === id);
          draggedAlbumId = '';
          if (from < 0 || to < 0 || from === to) return;
          const [photo] = albumPhotos.value.splice(from, 1);
          albumPhotos.value.splice(to, 0, photo);
          try { localStorage.setItem('eink.album.order.' + albumOffset.value, JSON.stringify(albumPhotos.value.map(p => p.id))); } catch (_) {}
        };
        const albumSelection = ref([]);
        const albumMounted = ref(false);
        const albumOffset = ref(0);
        const albumMore = ref(false);
        const albumStatus = ref('');
        let albumRevision = 0;

        const loadAlbum = async (offset = 0) => {
          const revision = ++albumRevision;
          try {
            const data = await fetchJson('/api/photos?offset=' + offset, {cache:'no-store'}, 45000);
            if (revision !== albumRevision) return;
            albumMounted.value = data.mounted;
            albumOffset.value = offset;
            albumMore.value = data.more;
            let order = [];
            try {
              const value = JSON.parse(localStorage.getItem('eink.album.order.' + offset) || '[]');
              if (Array.isArray(value)) order = value;
            } catch (_) {}
            const ranked = new Map(order.map((id, rank) => [id, rank]));
            albumPhotos.value = data.photos.map(id => ({id, thumbnail:''}))
              .sort((a, b) => (ranked.get(a.id) ?? Infinity) - (ranked.get(b.id) ?? Infinity));
            if (!data.photos.length && offset > 0) return loadAlbum(Math.max(0, offset - 12));
            if (!data.mounted) albumStatus.value = '请插入 SD 卡并重启设备';
            for (const photo of albumPhotos.value) {
              if (revision !== albumRevision || busy.value) break;
              try {
                const response = await fetch('/api/photos/preview?id=' + encodeURIComponent(photo.id));
                if (!response.ok) continue;
                const ids = new Uint8Array(await response.arrayBuffer());
                if (ids.length !== 48 * 72) continue;
                const thumb = document.createElement('canvas');
                thumb.width = 48;
                thumb.height = 72;
                const context = thumb.getContext('2d');
                for (let i = 0; i < ids.length; i++) {
                  context.fillStyle = COLORS.find(c => c.id === ids[i])?.css || '#fff';
                  context.fillRect(i % 48, Math.floor(i / 48), 1, 1);
                }
                photo.thumbnail = thumb.toDataURL('image/png');
              } catch (_) {}
            }
          } catch (_) {
            if (revision === albumRevision) albumStatus.value = '读取 SD 相册失败，请重新打开图片页';
          }
        };

        const saveAlbumFrame = async (frame, file, rotation) => {
          const upload = new Blob(file ? [frame.body, file] : [frame.body]);
          const until = Date.now() + 120000;
          while (true) {
            const response = await fetch('/api/photos', {method:'POST', headers:{
              'Content-Type':'application/octet-stream', 'X-Eink-Checksum':frame.checksum,
              'X-Eink-Original-Size':String(file?.size || 0), 'X-Eink-Photo-Rotation':String(rotation),
            }, body:upload, signal:AbortSignal.timeout(100000)});
            if (response.ok) {
              const result = await response.json();
              if (!result.ok || !result.id) throw new Error('设备未确认保存照片');
              return result.id;
            }
            const detail = await response.text();
            const message = response.headers.get('Content-Type')?.includes('application/json')
              ? JSON.parse(detail).message : detail;
            if (response.status !== 409 || !message?.includes('屏幕正忙') || Date.now() >= until)
              throw new Error(message || ('HTTP ' + response.status));
            await new Promise(resolve => setTimeout(resolve, 2000));
          }
        };

        const displayAlbumFrame = async id => {
          const job = await fetchJson('/api/photos/show', {method:'POST',
            headers:{'Content-Type':'text/plain'}, body:id}, 7000);
          const deadline = Date.now() + 90000;
          while (Date.now() < deadline) {
            const report = await fetchJson('/api/job', {cache:'no-store'}, 10000);
            if (report.id !== job.id || report.phase === 'error') throw new Error('设备刷新失败');
            if (report.phase === 'idle') return report;
            await new Promise(resolve => setTimeout(resolve, 1000));
          }
          throw new Error('等待刷新完成超时');
        };

        const saveEditedPhoto = async () => {
          if (busy.value || !imageEl.value || regionUpdate.value) return;
          busy.value = true;
          phase.value = 'prepare';
          status.value = '正在生成并保存调整后的画面…';
          clearTimeout(ditherTimer);
          ++previewRevision;
          ++imageRevision;
          try {
            await nextTick();
            const ids = await renderImageDithered();
            if (!ids) throw new Error('图像已变化，请重试');
            const frame = Pipeline.makeFrame(ids, ditherQuality.value);
            await saveAlbumFrame(frame, imageSource, panelRotation.value);
            if (albumFiles.value.length === 1 && albumFiles.value[0] === imageSource)
              albumFiles.value = [];
            phase.value = 'done';
            status.value = '调整后的画面已保存到 SD 相册';
            albumStatus.value = '可从相册选择保存后的照片';
          } catch (err) {
            phase.value = 'error';
            status.value = '保存失败 · ' + err.message;
          } finally {
            busy.value = false;
            await loadAlbum(albumOffset.value);
          }
        };

        const uploadAlbumFiles = async () => {
          if (busy.value || !albumFiles.value.length) return;
          if (albumFiles.value.length === 1 && albumFiles.value[0] === imageSource && imageEl.value)
            return saveEditedPhoto();
          busy.value = true;
          const files = albumFiles.value.slice();
          const failures = [];
          let complete = 0;
          try {
            const panel = await fetchJson('/api/status', {cache:'no-store'}, 5000);
            const image = document.createElement('canvas');
            image.width = 480;
            image.height = 720;
            const ctx = image.getContext('2d', {willReadFrequently:true});
            ctx.imageSmoothingQuality = 'high';
            Pipeline.setPanelRotation(0, panel.rotation_epoch);
            try {
              for (const file of files) {
                albumStatus.value = '正在上传 ' + (complete + failures.length + 1) + '/' + files.length + ' · ' + file.name;
                try {
                  const bitmap = await createImageBitmap(file);
                  try {
                    const landscape = bitmap.width > bitmap.height;
                    const outputWidth = landscape ? bitmap.height : bitmap.width;
                    const outputHeight = landscape ? bitmap.width : bitmap.height;
                    const factor = Math.max(480 / outputWidth, 720 / outputHeight);
                    const width = bitmap.width * factor, height = bitmap.height * factor;
                    ctx.fillStyle = '#fff';
                    ctx.fillRect(0, 0, 480, 720);
                    ctx.save();
                    try {
                      ctx.translate(240, 360);
                      if (landscape) ctx.rotate(Math.PI / 2);
                      ctx.drawImage(bitmap, -width / 2, -height / 2, width, height);
                    } finally {
                      ctx.restore();
                    }
                  } finally { bitmap.close(); }
                  const rgba = ctx.getImageData(0, 0, 480, 720).data;
                  const quality = ditherQuality.value;
                  const prepared = await Pipeline.quantizeAsync(rgba, {quality, style:renderStyle.value,
                    gray:quant.value === 'gray', denoise:sixDenoise.value, ...inkOptions()});
                  const frame = Pipeline.makeFrame(prepared.ids, quality);
                  await saveAlbumFrame(frame, file, 0);
                  complete++;
                } catch (err) {
                  failures.push({file, message:err.message});
                }
              }
            } finally {
              Pipeline.setPanelRotation(panel.rotation, panel.rotation_epoch);
            }
            albumFiles.value = failures.map(f => f.file);
            albumStatus.value = '已保存 ' + complete + ' 张' +
              (failures.length ? '，失败 ' + failures.length + ' 张 · ' + failures[0].file.name + '：' + failures[0].message : '，请选择照片传输');
          } catch (err) {
            albumStatus.value = '上传失败 · ' + err.message;
          } finally {
            busy.value = false;
            await loadAlbum(albumOffset.value);
          }
        };

        let albumEditRevision = 0;
        const openAlbumEditor = async () => {
          const revision = ++albumEditRevision;
          ++imageRevision;
          imageEl.value = null;
          imageName.value = '';
          regionUpdate.value = false;
          if (albumSelection.value.length !== 1) return;
          const id = albumSelection.value[0];
          albumStatus.value = '正在打开照片设置…';
          try {
            const base = '/api/photos/preview?id=' + encodeURIComponent(id);
            const original = await fetch(base + '&original=1', {signal:AbortSignal.timeout(90000)});
            if (original.ok) {
              const blob = await original.blob();
              if (revision !== albumEditRevision) return;
              const file = new File([blob], id.replace(/\.E6P$/i, ''), {type:blob.type});
              await loadFile(file, true);
              if (revision === albumEditRevision && imageEl.value) {
                if (imgW.value > imgH.value) rotateImage(90);
                albumStatus.value = '已打开原图，可直接传输或调整后发送';
                return;
              }
            }
            const response = await fetch(base + '&full=1',
              {signal:AbortSignal.timeout(55000)});
            if (!response.ok) throw new Error('无法读取照片');
            const frame = new Uint8Array(await response.arrayBuffer());
            const rotation = Number(response.headers.get('X-Eink-Photo-Rotation'));
            if (frame.length !== 172800 || ![0,90,180,270].includes(rotation))
              throw new Error('照片格式错误');
            if (revision !== albumEditRevision) return;
            const source = document.createElement('canvas');
            source.width = rotation % 180 ? 720 : 480;
            source.height = rotation % 180 ? 480 : 720;
            const ctx = source.getContext('2d');
            const pixels = ctx.createImageData(source.width, source.height);
            const palette = COLORS.map(({css, id}) => [id, Number.parseInt(css.slice(1), 16)]);
            const rgb = Object.fromEntries(palette);
            for (let x = 0; x < 480; ++x) {
              for (let y = 0; y < 720; ++y) {
                const pair = frame[(479 - x) * 360 + (y >> 1)];
                const value = rgb[y & 1 ? pair & 15 : pair >> 4] ?? 0xffffff;
                const sx = rotation === 90 ? y : rotation === 180 ? 479 - x : rotation === 270 ? 719 - y : x;
                const sy = rotation === 90 ? 479 - x : rotation === 180 ? 719 - y : rotation === 270 ? x : y;
                const i = (sy * source.width + sx) * 4;
                pixels.data[i] = value >> 16;
                pixels.data[i + 1] = (value >> 8) & 255;
                pixels.data[i + 2] = value & 255;
                pixels.data[i + 3] = 255;
              }
            }
            ctx.putImageData(pixels, 0, 0);
            const blob = await new Promise(resolve => source.toBlob(resolve, 'image/png'));
            if (revision !== albumEditRevision) return;
            if (!blob) throw new Error('无法生成图片预览');
            await loadFile(new File([blob], id + '.png', {type:'image/png'}), true);
            if (revision === albumEditRevision) albumStatus.value = '可直接传输，或调整后发送';
          } catch (err) {
            if (revision === albumEditRevision) albumStatus.value = '打开照片设置失败 · ' + err.message;
          }
        };

        const toggleAlbumPhoto = id => {
          const selected = albumSelection.value;
          albumSelection.value = selected.includes(id) ? selected.filter(x => x !== id) : [...selected, id];
          openAlbumEditor();
        };

        const deleteAlbumPhotos = async () => {
          const ids = albumSelection.value.slice();
          if (busy.value || !ids.length || !confirm('确定从 SD 卡删除选中的 ' + ids.length + ' 张照片吗？')) return;
          busy.value = true;
          ++albumEditRevision;
          ++imageRevision;
          imageEl.value = null;
          imageName.value = '';
          regionUpdate.value = false;
          let deleted = 0;
          try {
            for (let at = 0; at < ids.length; at += 24) {
              const batch = ids.slice(at, at + 24);
              const result = await fetchJson('/api/photos', {method:'DELETE',
                headers:{'Content-Type':'application/json'}, body:JSON.stringify(batch)}, 55000);
              const removed = batch.filter(id => !result.failed.includes(id));
              albumSelection.value = albumSelection.value.filter(id => !removed.includes(id));
              deleted += result.deleted;
            }
            albumStatus.value = '已删除 ' + deleted + ' 张' +
              (albumSelection.value.length ? '，还有 ' + albumSelection.value.length + ' 张未删除' : '');
          } catch (err) {
            albumStatus.value = '已删除 ' + deleted + ' 张，操作中断 · ' + err.message;
          } finally {
            busy.value = false;
            await loadAlbum(albumOffset.value);
          }
        };

        const showAlbumPhoto = async () => {
          if (busy.value || albumSelection.value.length !== 1) return;
          busy.value = true;
          ++albumEditRevision;
          albumStatus.value = '正在刷新墨水屏…';
          try {
            await displayAlbumFrame(albumSelection.value[0]);
            lastPanelFrame.value = null;
            regionUpdate.value = false;
            albumStatus.value = '所选照片已刷新到屏幕';
          } catch (err) {
            albumStatus.value = err.message;
          } finally {
            busy.value = false;
          }
        };

        const weatherQuery = ref('');
        const weatherName = ref('');
        const weatherLat = ref('');
        const weatherLon = ref('');
        const weatherStation = ref('');
        const weatherPlaces = ref([]);
        const weatherSelection = ref(null);
        const savedWeatherLocation = ref(null);
        const weatherStatus = ref('');
        const weatherBusy = ref(false);
        const calendarStatus = ref('');

        const loadWeatherLocation = async () => {
          try {
            const location = await fetchJson('/api/weather/location', { cache: 'no-store' }, 5000);
            savedWeatherLocation.value = location.name ? location : null;
          } catch (_) {
            weatherStatus.value = '无法读取设备上的天气地点';
          }
        };
        const searchWeatherPlaces = async () => {
          if (weatherBusy.value || busy.value) return;
          weatherBusy.value = true;
          weatherSelection.value = null;
          weatherPlaces.value = [];
          weatherStatus.value = '正在搜索地点…';
          try {
            weatherPlaces.value = await window.HueLeafWeather.search(weatherQuery.value, fetchJson);
            weatherStatus.value = weatherPlaces.value.length ? '请选择地点' : '没有匹配地点，可以输入经纬度';
          } catch (_) {
            weatherStatus.value = '地点搜索失败，可以手动输入经纬度';
          } finally {
            weatherBusy.value = false;
          }
        };
        const saveWeatherLocation = async ({ name, latitude, longitude, stationPath }) => {
          if (weatherBusy.value || busy.value) return;
          if (!name || !Number.isFinite(latitude) || !Number.isFinite(longitude)) {
            weatherStatus.value = '请填写地点名称和有效经纬度';
            return;
          }
          weatherBusy.value = true;
          try {
            const location = { name, latitude, longitude, bitmap: window.HueLeafWeather.nameBitmap(name) };
            if (stationPath) {
              const url = new URL(stationPath);
              if (!['www.nmc.cn', 'nmc.cn'].includes(url.hostname) || url.protocol !== 'https:' ||
                  !url.pathname.startsWith('/publish/forecast/') || !url.pathname.endsWith('.html')) {
                throw new Error('请输入中央气象台城市预报页的 HTTPS 地址');
              }
              location.stationPath = url.pathname;
            }
            await fetchJson('/api/weather/location', {
              method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(location),
            }, 5000);
            savedWeatherLocation.value = location;
            weatherStatus.value = '地点已保存，点击更新天气并刷屏';
          } catch (_) {
            weatherStatus.value = '保存地点失败，请检查名称、经纬度和预报页地址';
          } finally {
            weatherBusy.value = false;
          }
        };
        const weatherOffline = ref(false);
        const weatherPolicyLoaded = ref(false);
        const weatherPolicyBusy = ref(false);
        const loadWeatherPolicy = async () => {
          weatherPolicyLoaded.value = false;
          try {
            const policy = await fetchJson('/api/weather/policy', { cache: 'no-store' }, 5000);
            weatherOffline.value = policy.offline;
            weatherPolicyLoaded.value = true;
          } catch (_) { weatherStatus.value = '无法读取天气联网设置'; }
        };
        const setWeatherOffline = async (enabled) => {
          weatherPolicyBusy.value = true;
          const previous = weatherOffline.value;
          weatherOffline.value = enabled;
          try {
            const policy = await fetchJson('/api/weather/policy', {
              method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: String(enabled),
            }, 5000);
            weatherOffline.value = policy.offline;
          } catch (_) {
            weatherOffline.value = previous;
            weatherStatus.value = '天气联网设置保存失败';
          } finally { weatherPolicyBusy.value = false; }
        };
        const quoteFields = ['first', 'second', 'title', 'author', 'url'];
        const quoteHeader = ['摘句上行', '摘句下行', '书名', '作者', '原文链接'].join('\t');
        const emptyQuote = () => Object.fromEntries(quoteFields.map(field => [field, '']));
        const quoteDraft = ref(emptyQuote());
        const quoteEntries = ref([]);
        const quotesLoaded = ref(false);
        const quoteBusy = ref(false);
        const quoteStatus = ref('');
        const encodeQuote = (entry) => {
          const fields = quoteFields.map(field => entry[field].trim());
          if (fields.slice(0, 4).some(field => !field || /[\t\r\n]/.test(field)) ||
              (fields[4] && !/^https:\/\/[^\s]+$/.test(fields[4]))) {
            throw new Error('请填写两行摘句、书名和作者。原文链接请使用 HTTPS。');
          }
          return fields.join('\t');
        };
        const parseQuoteFile = (content, strict) => {
          const entries = [];
          for (const [index, line] of content.replace(/^\uFEFF/, '').split(/\r?\n/).entries()) {
            if (!line.trim() || line === quoteHeader) continue;
            const fields = line.split('\t');
            if (fields.length === 4) fields.push('');
            if (fields.length !== 5) {
              if (strict) throw new Error('第 ' + (index + 1) + ' 行需要用制表符分隔各列。');
              continue;
            }
            entries.push(Object.fromEntries(quoteFields.map((field, i) => [field, fields[i]])));
          }
          return entries;
        };
        const readQuotes = async () => {
          const response = await fetch('/api/quotes', { cache: 'no-store', signal: AbortSignal.timeout(6000) });
          if (!response.ok) throw new Error('无法读取句库，请确认设备已挂载 SD 卡。');
          quoteEntries.value = parseQuoteFile(await response.text(), false);
          quotesLoaded.value = true;
        };
        const runQuoteAction = async (action) => {
          quoteBusy.value = true;
          quoteStatus.value = '';
          try {
            quoteStatus.value = await action();
          } catch (error) {
            quoteStatus.value = error.message || '操作失败，请检查设备连接。';
          } finally {
            quoteBusy.value = false;
          }
        };
        const loadQuotes = () => runQuoteAction(async () => {
          await readQuotes();
          return '';
        });
        const saveQuotes = async (entries) => {
          const body = [...new Set(entries.map(encodeQuote))].join('\n');
          if (!body) throw new Error('文件里没有可导入的摘句。');
          if (new TextEncoder().encode(body).length >= 8192) throw new Error('请将摘句分成小于 8 KB 的批次导入。');
          await fetchJson('/api/quotes', {
            method: 'POST', headers: {'Content-Type': 'text/plain; charset=utf-8'}, body,
          }, 8000);
          await readQuotes();
        };
        const addQuote = () => runQuoteAction(async () => {
          await saveQuotes([quoteDraft.value]);
          quoteDraft.value = emptyQuote();
          return '摘句已保存到 SD 卡。';
        });
        const importQuotes = (event) => {
          const file = event.target.files[0];
          event.target.value = '';
          if (!file) return;
          return runQuoteAction(async () => {
            const before = quoteEntries.value.length;
            await saveQuotes(parseQuoteFile(await file.text(), true));
            return '新增 ' + (quoteEntries.value.length - before) + ' 条摘句。';
          });
        };
        const downloadQuoteTemplate = () => {
          const example = '有朋自远方来\t不亦乐乎\t《论语·学而》\t孔子及其弟子\thttps://ctext.org/analects/xue-er/zhs';
          const url = URL.createObjectURL(new Blob(['\uFEFF', quoteHeader, '\r\n', example, '\r\n'],
            { type: 'text/tab-separated-values;charset=utf-8' }));
          const link = document.createElement('a');
          link.href = url;
          link.download = '每日一句导入模板.tsv';
          link.click();
          setTimeout(() => URL.revokeObjectURL(url), 1000);
        };
        const showQuote = (entry) => runQuoteAction(async () => {
          await fetchJson(entry ? '/api/quotes/show' : '/api/display/refresh', {
            method: 'POST', headers: {'Content-Type': 'text/plain; charset=utf-8'},
            body: entry ? encodeQuote(entry) : 'quote',
          }, 6000);
          return '已提交，等墨水屏完成刷新。';
        });
        const trips = ref([]);
        const tripsLoaded = ref(false);
        const tripBusy = ref(false);
        const tripStatus = ref('');
        const tripEditing = ref(null);
        const tripDraft = ref({});
        const tripPlaces = ref([]);
        const beijingNow = () => new Date(Date.now() + 8 * 3600000).toISOString();
        const newTrip = () => {
          tripDraft.value = { kind:'train', date:beijingNow().slice(0,10), arrival_date:'', depart:'', arrive:'',
            number:'', from:'', to:'', city:'', seat:'', gate:'', note:'', latitude:'', longitude:'' };
          tripPlaces.value = [];
          tripEditing.value = -1;
        };
        const editTrip = index => {
          tripDraft.value = {...trips.value[index]};
          tripPlaces.value = [];
          tripEditing.value = index;
        };
        const runTrip = async action => {
          tripBusy.value = true;
          tripStatus.value = '';
          try { tripStatus.value = await action(); }
          catch (error) { tripStatus.value = error.message || '行程操作失败。'; }
          finally { tripBusy.value = false; }
        };
        const loadTrips = () => runTrip(async () => {
          trips.value = await fetchJson('/api/trips', {cache:'no-store'}, 5000);
          tripsLoaded.value = true;
          return '';
        });
        const storeTrips = async next => {
          const entries = next.map(({weather_cached, weather_updated, trip_id, selected, ...ticket}) => ticket);
          await fetchJson('/api/trips', {method:'POST', headers:{'Content-Type':'application/json'},
            body:JSON.stringify(entries)}, 7000);
          trips.value = await fetchJson('/api/trips', {cache:'no-store'}, 5000);
        };
        const saveTrip = () => runTrip(async () => {
          if (!Number.isFinite(tripDraft.value.latitude) || !Number.isFinite(tripDraft.value.longitude)) {
            const places = await window.HueLeafWeather.search(tripDraft.value.city, fetchJson);
            tripPlaces.value = places;
            return places.length ? '请选择匹配的地点，再保存行程。' : '未找到地点，请输入更长的地名前缀或填写经纬度。';
          }
          const ticket = {...tripDraft.value};
          ticket.arrival_date ||= ticket.date;
          const next = [...trips.value];
          if (tripEditing.value === -1) next.push(ticket);
          else next[tripEditing.value] = ticket;
          await storeTrips(next);
          tripEditing.value = null;
          return '行程已保存。设备联网时会缓存七天内的抵达日预报。';
        });
        const removeTrip = index => runTrip(async () => {
          await storeTrips(trips.value.filter((_, i) => i !== index));
          tripEditing.value = null;
          return '已删除行程。';
        });
        const selectTrip = id => runTrip(async () => {
          const result = await fetchJson('/api/trips/select', {
            method:'POST', headers:{'Content-Type':'text/plain'}, body:id,
          }, 5000);
          trips.value = await fetchJson('/api/trips', {cache:'no-store'}, 5000);
          return result.queued ? '已提交，设备将刷新行程页面。' : '显示选择已保存。';
        });
        const findTripCity = () => runTrip(async () => {
          tripPlaces.value = await window.HueLeafWeather.search(tripDraft.value.city, fetchJson);
          return tripPlaces.value.length ? '' : '未找到地点，可以手动填写经纬度。';
        });
        const resetTripCity = async (event) => {
          tripDraft.value.latitude = '';
          tripDraft.value.longitude = '';
          tripPlaces.value = [];
          const draft = tripDraft.value, query = event.target.value.trim();
          if (!query) return;
          const places = await window.HueLeafWeather.localSearch(query, fetchJson);
          if (tripEditing.value !== null && draft === tripDraft.value && query === draft.city)
            tripPlaces.value = places;
        };
        const chooseTripCity = place => {
          Object.assign(tripDraft.value, {city:place.name, latitude:place.latitude, longitude:place.longitude});
          tripPlaces.value = [];
        };
        const historyItems = ref([]);
        const historyBusy = ref(false);
        const historyProviderId = ref('');
        const historyProviderKey = ref('');
        const saveHistoryProvider = async () => {
          if (historyBusy.value) return;
          historyBusy.value = true;
          try {
            await fetchJson('/api/history/provider', { method:'POST',
              body:JSON.stringify({id:historyProviderId.value, key:historyProviderKey.value}) }, 5000);
            historyProviderKey.value = '';
            historyStatus.value = '已保存，设备将补抓缺失日期';
          } catch (error) {
            historyStatus.value = '历史数据源保存失败：' + error.message;
          } finally { historyBusy.value = false; }
        };
        const historyStatus = ref('');
        const loadHistory = async () => {
          historyBusy.value = true;
          historyStatus.value = '';
          try {
            const data = await fetchJson('/api/history', {cache:'no-store'}, 5000);
            historyItems.value = data.items;
            if (!data.items.length) historyStatus.value = '等待设备校时。';
          } catch (error) { historyStatus.value = error.message; }
          finally { historyBusy.value = false; }
        };
        const shortcutMode = ref(5);
        const shortcutBusy = ref(false);
        const shortcutLoaded = ref(false);
        const shortcutStatus = ref('');
        const loadShortcut = async () => {
          shortcutBusy.value = true;
          try {
            shortcutMode.value = (await fetchJson('/api/display/shortcut', {cache:'no-store'}, 5000)).mode;
            shortcutLoaded.value = true;
          } catch (error) { shortcutStatus.value = error.message; }
          finally { shortcutBusy.value = false; }
        };
        const saveShortcut = async () => {
          shortcutBusy.value = true;
          try {
            await fetchJson('/api/display/shortcut', {method:'POST', body:String(shortcutMode.value)}, 5000);
            shortcutStatus.value = '快捷页面已保存。';
          } catch (error) { shortcutStatus.value = error.message; }
          finally { shortcutBusy.value = false; }
        };
        const lunarYear = ref(Number(beijingNow().slice(0,4)));
        const lunarBusy = ref(false);
        const lunarStatus = ref('');
        const loadLunarScript = () => window.Solar ? Promise.resolve() : new Promise((resolve, reject) => {
          const script = document.createElement('script');
          script.src = '/lunar.js';
          script.onload = resolve;
          script.onerror = () => { script.remove(); reject(new Error('无法读取设备的农历计算模块。')); };
          document.head.appendChild(script);
        });
        const updateLunarYear = async () => {
          lunarBusy.value = true;
          lunarStatus.value = '正在生成全年数据';
          try {
            const year = Number(lunarYear.value);
            if (!Number.isInteger(year) || year < 2025 || year > 2099) throw new Error('请输入 2025 至 2099 年。');
            await loadLunarScript();
            const rows = [];
            for (const date = new Date(Date.UTC(year,0,1)); date.getUTCFullYear() === year; date.setUTCDate(date.getUTCDate()+1)) {
              const lunar = window.Solar.fromYmd(year,date.getUTCMonth()+1,date.getUTCDate()).getLunar();
              rows.push([date.toISOString().slice(0,10), lunar.getMonthInChinese()+'月'+lunar.getDayInChinese(),
                lunar.getDayYi().slice(0,3).join(' '), lunar.getDayJi().slice(0,3).join(' '), lunar.getJieQi()].join('\t'));
            }
            await fetchJson('/api/lunar', {method:'POST', headers:{'Content-Type':'text/plain; charset=utf-8'},
              body:rows.join('\n')+'\n'}, 15000);
            lunarStatus.value = year + ' 年农历已保存，共 ' + rows.length + ' 天。';
          } catch (error) { lunarStatus.value = error.message; }
          finally { lunarBusy.value = false; }
        };
        const todoItems = ref([]);
        const todoZoom = ref(125);
        const todoPreview = ref(null);
        const todoLoaded = ref(false);
        const todoBusy = ref(false);
        const todoStatus = ref('');
        let nextTodoId = 1;
        const addTodo = () => todoItems.value.push({ id: nextTodoId++, text: '', done: false, due: '' });
        const loadTodo = async () => {
          todoBusy.value = true;
          todoStatus.value = '正在读取待办';
          try {
            const result = await fetchJson('/api/todo', { cache: 'no-store' }, 5000);
            todoItems.value = result.items.map(item => ({ due:'', ...item, id: nextTodoId++ }));
            todoZoom.value = [100, 125, 150].includes(result.zoom) ? result.zoom : 125;
            todoLoaded.value = true;
            todoStatus.value = '';
          } catch (_) {
            todoStatus.value = '读取失败，请检查设备连接后重试。';
          } finally {
            todoBusy.value = false;
          }
        };
        const todoTextCanvas = (items, zoom) => {
          const canvas = document.createElement('canvas');
          const width = 352;
          const rowHeight = 24;
          canvas.width = width;
          canvas.height = rowHeight * items.length;
          const ctx = canvas.getContext('2d');
          ctx.textBaseline = 'middle';
          items.forEach((item, index) => {
            ctx.font = '600 22px "Microsoft YaHei", "PingFang SC", sans-serif';
            ctx.fillStyle = item.color || '#000';
            ctx.fillText(item.text, 0, (index + .5) * rowHeight, Math.floor((item.due ? 236 : width - 1) * 100 / zoom));
          });
          return canvas;
        };
        const todoBitmap = (items, zoom) => {
          if (!items.length) return '';
          const canvas = todoTextCanvas(items, zoom);
          const { width, height } = canvas;
          const ctx = canvas.getContext('2d');
          const { data } = ctx.getImageData(0, 0, width, height);
          const bytes = new Uint8Array(width * height / 8);
          for (let i = 0; i < width * height; i++) {
            if (data[i * 4 + 3] >= 96) bytes[i >> 3] |= 128 >> (i & 7);
          }
          return btoa(String.fromCharCode(...bytes));
        };
        const drawTodoPreview = () => {
          if (!todoPreview.value) return;
          const ctx = todoPreview.value.getContext('2d');
          const blue = '#205ab5', green = '#168061', black = '#171e27';
          const items = todoItems.value.map(item => ({ text: item.text.trim(), due:item.due, color: item.done ? green : black }));
          const count = items.length;
          const done = todoItems.value.filter(item => item.done).length;
          const rect = (x, y, w, h, color) => { ctx.fillStyle = color; ctx.fillRect(x, y, w, h); };
          const label = (value, x, y, size, color, align = 'left') => {
            ctx.fillStyle = color;
            ctx.font = '700 ' + size + 'px "Microsoft YaHei", "PingFang SC", sans-serif';
            ctx.textAlign = align;
            ctx.textBaseline = 'top';
            ctx.fillText(value, x, y);
          };
          rect(0, 0, 480, 720, '#fff');
          rect(28, 23, 5, 33, blue);
          label('待办事项', 45, 23, 30, black);
          label(done + ' / ' + count, 394, 32, 29, blue, 'center');
          const now = beijingNow();
          label(now.slice(11,16), 28, 64, 58, black);
          label(now.slice(5,10).replace('-', '/'), 318, 104, 16, blue);
          rect(28, 150, 424, 4, black);
          if (!count) {
            label('暂无待办', 240, 316, 44, black, 'center');
            label('请在网页管理', 240, 387, 28, blue, 'center');
            return;
          }
          rect(28, 150, Math.floor(424 * done / count), 4, green);
          const text = todoTextCanvas(items, todoZoom.value);
          const scale = todoZoom.value / 100;
          const height = 24 * scale;
          ctx.imageSmoothingEnabled = false;
          todoItems.value.forEach((item, index) => {
            const y = 186 + index * 91;
            rect(34, y + 18, 28, 28, item.done ? green : blue);
            if (item.done) {
              for (let j = 0; j < 6; j++) rect(40 + j, y + 31 + j, 3, 3, '#fff');
              for (let j = 0; j < 12; j++) rect(45 + j, y + 36 - j, 3, 3, '#fff');
            } else rect(37, y + 21, 22, 22, '#fff');
            const width = item.due ? 237 : 352;
            ctx.drawImage(text, 0, index * 24, width / scale, 24, 85, y + 32 - height / 2, width, height);
            if (item.due) label(item.due.slice(5,10) + ' ' + item.due.slice(11,16), 335, y + 24, 15, blue);
            for (let x = 28; x < 448; x += 14) rect(x, y + 70, 8, 1, black);
          });
        };
        watch([todoItems, todoZoom, todoPreview], drawTodoPreview, { deep: true, flush: 'post' });
        const saveTodo = async () => {
          const items = todoItems.value.map(({ text, done, due }) => ({ text: text.trim(), done, due }));
          if (items.some(item => !item.text)) {
            todoStatus.value = '请填写每项内容，或删除空白项。';
            return;
          }
          todoBusy.value = true;
          todoStatus.value = '正在保存待办';
          try {
            const result = await fetchJson('/api/todo', {
              method: 'POST', headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ items, zoom: todoZoom.value, bitmap: todoBitmap(items, todoZoom.value) }),
            }, 7000);
            todoStatus.value = result.queued ? '已保存，墨水屏正在排队刷新。' : '已保存，可在后台切换到待办页面。';
          } catch (_) {
            todoStatus.value = '保存失败，请检查设备连接后重试。';
          } finally {
            todoBusy.value = false;
          }
        };
        const activeScreen = ref('');
        const availableScreens = ref(0);
        const deviceScreenBusy = ref(false);
        const directStatus = ref('');
        const requestDisplay = async (mode, direct = false) => {
          const status = direct ? directStatus : mode === 'calendar' ? calendarStatus : weatherStatus;
          if (busy.value || weatherBusy.value) return;
          weatherBusy.value = true;
          try {
            await fetchJson('/api/display/refresh', {
              method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: mode,
            }, 5000);
            status.value = direct && deviceScreenBusy.value ? '已排队，当前刷新结束后切换' : '已提交，设备将刷新屏幕';
            checkDevice();
          } catch (error) {
            status.value = '刷新请求失败 · ' + (error.message || error);
          } finally {
            weatherBusy.value = false;
          }
        };

        const displayPages = ref([
          ['text', '文字'], ['photo', '照片'], ['calendar', '桌面日历'], ['weather', '天气时钟'],
          ['quote', '日历日签'], ['todo', '待办事项'], ['trip', '我的行程'], ['history', '历史今天'],
        ].map(([id, name]) => ({ id, name, enabled: true })));
        const monoPages = ref(0);
        const monoLoaded = ref(false);
        const monoBusy = ref(false);
        const monoStatus = ref('');
        const loadDisplayStyle = async () => {
          monoBusy.value = true;
          monoStatus.value = '';
          try {
            const style = await fetchJson('/api/display/style', { cache: 'no-store' }, 5000);
            monoPages.value = style.mono;
            monoLoaded.value = true;
          } catch (_) {
            monoLoaded.value = false;
            monoStatus.value = '无法读取页面显示设置，重新进入设置页可重试。';
          } finally { monoBusy.value = false; }
        };
        const toggleMono = async (index) => {
          if (monoBusy.value || !monoLoaded.value) return;
          monoBusy.value = true;
          monoStatus.value = '';
          try {
            const style = await fetchJson('/api/display/style', {
              method: 'POST', headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ mono: monoPages.value ^ (1 << index) }),
            }, 5000);
            monoPages.value = style.mono;
            monoStatus.value = '已保存，下次刷屏生效。';
          } catch (_) {
            monoStatus.value = '显示设置保存失败。';
          } finally { monoBusy.value = false; }
        };
        const carousel = ref({ running: false, every: 5 });
        const scheduleStatus = ref('');
        const scheduleBusy = ref(false);
        const scheduleBaseline = ref('');
        const scheduleSnapshot = () => JSON.stringify({
          running:carousel.value.running, every:carousel.value.every,
          pages:displayPages.value.map(page => page.enabled),
        });
        const scheduleDirty = computed(() => Boolean(scheduleBaseline.value) && scheduleBaseline.value !== scheduleSnapshot());
        const loadDisplaySchedule = async () => {
          scheduleBusy.value = true;
          try {
            const config = await fetchJson('/api/display/schedule', { cache: 'no-store' }, 5000);
            displayPages.value.forEach((page, index) => { page.enabled = Boolean(config.enabled & (1 << index)); });
            carousel.value = { running: config.running, every: config.every };
            scheduleBaseline.value = scheduleSnapshot();
          } catch (_) {
            scheduleStatus.value = '无法读取设备上的刷新计划';
          } finally {
            scheduleBusy.value = false;
          }
        };
        const saveDisplaySchedule = async () => {
          if (scheduleBusy.value) return;
          let enabled = 0;
          displayPages.value.forEach((page, index) => { if (page.enabled) enabled |= 1 << index; });
          if (!Number.isInteger(Number(carousel.value.every)) || Number(carousel.value.every) < 1 || Number(carousel.value.every) > 1439) {
            scheduleStatus.value = '切换间隔必须为 1 至 1439 分钟';
            return;
          }
          if (!enabled) {
            scheduleStatus.value = '至少选择一个切换页面';
            return;
          }
          scheduleBusy.value = true;
          try {
            await fetchJson('/api/display/schedule', { method: 'POST',
              headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ enabled, running: carousel.value.running, every: Number(carousel.value.every) }),
            }, 5000);
            scheduleBaseline.value = scheduleSnapshot();
            scheduleStatus.value = '刷新计划已保存到设备';
          } catch (_) {
            scheduleStatus.value = '刷新计划保存失败';
          } finally {
            scheduleBusy.value = false;
          }
        };

        let statusInFlight = false;
        const checkDevice = async () => {
          if (statusInFlight || busy.value) return;
          statusInFlight = true;
          try {
            const info = await fetchJson('/api/status', { cache: 'no-store' }, 5000);
            deviceOnline.value = !!(info.ok && info.w === 480 && info.h === 720 && info.format === Pipeline.FORMAT);
            if (deviceOnline.value && Date.now() - wifiOffAt > 2000) wifiOffRequested.value = false;
            batteryLevel.value = info.gauge_read_ok && info.gauge_enabled && info.battery_present &&
              Number.isInteger(info.soc) && info.soc >= 0 && info.soc <= 100 ? info.soc : null;
            lowBattery.value = Boolean(info.low_battery);
            internetConfirmed.value = Boolean(info.internet_confirmed);
            statusDetails.value = info;
            activeScreen.value = info.active_screen || '';
            availableScreens.value = info.available_screens || 0;
            deviceScreenBusy.value = Boolean(info.rotation_busy || info.job === 'refresh' || info.job === 'receiving');
            if (info.mode) wifiMode.value = info.mode;
            if (info.ssid) wifiSsid.value = info.ssid;
            if (info.url) wifiUrl.value = info.url;
            if (deviceOnline.value && !info.rotation_busy) {
 const rotation=info.rotation??0,w=rotation%180?720:480,h=rotation%180?480:720;
 Pipeline.setPanelRotation(rotation,info.rotation_epoch??1);
 if(panelRotation.value!==rotation || PANEL_W!==w){
 PANEL_W=w;PANEL_H=h;panelWidth.value=w;panelHeight.value=h;panelRotation.value=rotation;
 scratch.width=w;scratch.height=h;canvas.value.width=w;canvas.value.height=h;
 lastPanelFrame.value=null;regionUpdate.value=false;updateRect.value={x:0,y:0,width:w,height:h};
 await nextTick();if(tab.value==='image')scheduleDither();else renderText();
 }
 }
            if (info.charge && !chargeBusy.value) applyChargeInfo(info);
            if (tab.value === 'wifi') void loadSync();
          } catch (_) {
            deviceOnline.value = false;
          } finally {
            statusInFlight = false;
          }
        };

        const scanWifi = async () => {
          if (wifiBusy.value) return;
          wifiBusy.value = true;
          wifiStatus.value = '扫描中';
          wifiStatusClass.value = '';
          try {
            const info = await fetchJson('/api/wifi/scan', { cache: 'no-store' }, 20000);
            wifiList.value = (info.aps || []).filter((x) => x.ssid);
            wifiStatus.value = wifiList.value.length ? `找到 ${wifiList.value.length} 个网络` : '没有扫到网络';
          } catch (e) {
            wifiStatusClass.value = 'err';
            wifiStatus.value = '扫描失败，靠近路由器再试';
          }
          wifiBusy.value = false;
        };

        const selectWifi = ssid => {
          if (wifiBusy.value) return;
          if (selectedSsid.value !== ssid) wifiPass.value = '';
          selectedSsid.value = ssid;
        };

        const setChargePower = async (id) => {
          if (!chargeLevels.some(level => level.id === id) || chargeBusy.value || !chargeReady.value) return;
          if (!Number.isInteger(chargeCapacity.value) || chargeCapacity.value < 500 || chargeCapacity.value > 10000) {
            chargeStatus.value = '请输入 500–10000 mAh 范围内的整数容量';
            chargeStatusClass.value = 'err';
            return;
          }
          chargeBusy.value = true;
          chargeStatusClass.value = '';
          chargeStatus.value = '设置中';
          try {
            const info = await fetchJson('/api/charge', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ power: id, capacity_mah: chargeCapacity.value }),
            });
            applyChargeInfo(info, true);
            chargeStatus.value = `已设为 ${info.name || id}`;
            chargeStatusClass.value = 'ok';
          } catch (e) {
            chargeStatusClass.value = 'err';
            chargeStatus.value = '设置失败，稍后重试';
          }
          chargeBusy.value = false;
        };

        const connectWifi = async () => {
          if (!selectedSsid.value || wifiBusy.value) return;
          wifiBusy.value = true;
          wifiStatusClass.value = '';
          wifiStatus.value = '连接中';
          try {
            await fetchJson('/api/wifi/connect', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ ssid: selectedSsid.value, password: wifiPass.value || '' }),
            });
            wifiStatus.value = '已提交。手机改连该 Wi-Fi，再扫屏幕上的二维码。';
            wifiStatusClass.value = 'ok';
          } catch (e) {
            wifiStatusClass.value = 'err';
            wifiStatus.value = '提交失败，设备可能已切换网络';
          }
          wifiBusy.value = false;
        };

        onMounted(() => {
          restoreTheme();
          void loadPalette();
          void loadSync(true);
          renderText();
          checkDevice();
          let lastStatusPoll = 0;
          setInterval(() => {
            if (document.hidden) return;
            const now = Date.now();
            if (now - lastStatusPoll >= (busy.value || deviceScreenBusy.value ? 1000 : 10000)) {
              lastStatusPoll = now;
              checkDevice();
            }
          }, 1000);
          document.addEventListener('visibilitychange', () => { if (!document.hidden) checkDevice(); });
          const frame = canvas.value && canvas.value.parentElement;
          if (frame) frame.addEventListener('wheel', onWheel, { passive: false });
          document.addEventListener('paste', (ev) => {
            const item = Array.from(ev.clipboardData?.items || []).find((x) => x.type.startsWith('image/'));
            if (item) loadFile(item.getAsFile());
          });
        });

        return {
          albumFiles, albumPhotos, beginAlbumDrag, dropAlbumPhoto, albumSelection, albumMounted, albumStatus,
          albumOffset, albumMore, loadAlbum, uploadAlbumFiles, showAlbumPhoto, toggleAlbumPhoto, deleteAlbumPhotos, saveEditedPhoto,
          weatherQuery, weatherName, weatherLat, weatherLon, weatherStation, weatherPlaces, weatherSelection,
          savedWeatherLocation, weatherStatus, weatherBusy, searchWeatherPlaces, saveWeatherLocation,
          requestDisplay, calendarStatus, directStatus, activeScreen, availableScreens, deviceScreenBusy, displayPages, carousel, scheduleStatus, scheduleBusy, scheduleDirty, saveDisplaySchedule,
          monoPages, monoLoaded, monoBusy, monoStatus, toggleMono,
          todoItems, todoZoom, todoPreview, todoLoaded, todoBusy, todoStatus, addTodo, loadTodo, saveTodo,
          trips, tripsLoaded, tripBusy, tripStatus, tripEditing, tripDraft, tripPlaces, newTrip, editTrip,
          loadTrips, saveTrip, removeTrip, selectTrip, findTripCity, chooseTripCity, resetTripCity, historyItems, historyBusy, historyStatus, loadHistory, historyProviderId, historyProviderKey, saveHistoryProvider,
          shortcutMode, shortcutBusy, shortcutLoaded, shortcutStatus, saveShortcut,
          lunarYear, lunarBusy, lunarStatus, updateLunarYear,
          quoteDraft, quoteEntries, quotesLoaded, quoteBusy, quoteStatus, loadQuotes, addQuote,
          importQuotes, downloadQuoteTemplate, showQuote,
          weatherOffline, weatherPolicyLoaded, weatherPolicyBusy, setWeatherOffline,
          panelWidth, panelHeight, panelRotation, tab, text, bg, fg, fontSize, busy, busyLabel, status, statusClass, colors,
          canvas, appearanceCanvas, fileInput, imageName, imageEl, imgW, imgH, quant, dragging,
          inkColors, inkAlgorithm, inkStrength, inkCap, inkSerpentine, conversionProgress, photoPreset, showOriginal,
          paletteMessage, savePalette, restorePalette, calibrationBusy, calibrationPatch, calibrationPhoto, sampleTarget, openCalibrationPhoto, sampleCalibration, inkEngine, advancedInk, scheduleDither, syncStatus, statusDetails, syncMessage, syncActionBusy, syncStart, syncEnd,
          formatSyncTime, syncLabel, syncNow, saveSyncWindow,
          previewPixels, noisePreviews, noisePreviewBusy, ditherQuality, ditherModes, sixDenoise, sixDenoiseModes, setSixDenoise, onNoiseCompareToggle,
          deviceOnline, batteryLevel, lowBattery, internetConfirmed, renderStyle, renderStyles, previewBusy, usedColors, draggingFile,
          wifiPowerBusy, wifiOffRequested, turnOffWifi,
          zoom, panX, panY, exposure, brightness, contrast, highlights, shadows, saturation,
          wifiMode, wifiSsid, wifiUrl, wifiList, selectedSsid, wifiPass, wifiBusy, wifiStatus, wifiStatusClass,
          chargeCapacity, chargeDirty, chargeReady, chargeDetail, gaugeDetail, chargePower, chargeBusy, chargeStatus, chargeStatusClass, chargeLevels,
          switchTab, renderText, onFileChange, onDrop, push, setQuant, setDitherQuality, setRenderStyle,
          clearDisplay,
          lastPanelFrame, regionUpdate, forceRefresh, updateRect, canRegionUpdate, setRegionUpdate,
          onCropChange, onAdjustChange, resetCrop, resetAdjust, rotateImage, imageRotation,
          onPanStart, onPanMove, onPanEnd, scanWifi, selectWifi, connectWifi, setChargePower,
          themes, theme, themeName, setTheme,
        };
      },
    }).mount('#app');
