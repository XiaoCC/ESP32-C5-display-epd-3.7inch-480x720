(() => {
  let catalog;
  let loading;
  async function localSearch(name, fetchJson) {
    const [prefix, qualifier = ''] = name.trim().replace(/，/g, ',').split(',').map(value => value.trim());
    if (!prefix) return [];
    if (!catalog) {
      loading ||= fetchJson('/place-catalog.json', {}, 5000)
        .then(records => { catalog = records; return records; })
        .finally(() => { loading = null; });
      try { await loading; } catch (_) { return []; }
    }
    const matches = [];
    for (const [code, name, province, city, latitude, longitude] of catalog) {
      if (qualifier && ![province, city, '中国', 'CN'].some(value =>
        value && value.toLowerCase().startsWith(qualifier.toLowerCase()))) continue;
      const rank = name === prefix ? 0 : name.startsWith(prefix) ? 1 :
        city.startsWith(prefix) ? 2 :
        (city + name).startsWith(prefix) || (city.replace(/市$/, '') + name).startsWith(prefix) ? 3 : 4;
      if (rank < 4) matches.push({ id: 'cn-' + code, name, latitude, longitude,
        detail: [...new Set([province, city].filter(value => value && value !== name))].join(' · '), rank });
    }
    return matches.sort((a,b) => a.rank - b.rank || a.id.localeCompare(b.id)).slice(0, 15);
  }

  async function search(name, fetchJson) {
    const input = name.trim().replace(/，/g, ',');
    if (!input) return [];
    const local = await localSearch(input, fetchJson);
    if (local.length) return local;
    // GeoNames 1787227 via Open-Meteo. Keep the device's default city usable from its offline hotspot.
    if (/^扬州(?:市)?(?:\s*,\s*(?:江苏(?:省)?|中国|CN))?$/i.test(input)) {
      return [{ id: 1787227, name: '扬州市', latitude: 32.39722, longitude: 119.43583, detail: '江苏 · 中国' }];
    }
    const match = /^([\u3400-\u9fff]+)(\s*,.*)?$/.exec(input);
    const suffixed = match && !/[市县区旗盟]$/.test(match[1])
      ? match[1] + '市' + (match[2] || '') : '';
    const query = name => 'https://geocoding-api.open-meteo.com/v1/search?' +
      new URLSearchParams({ name, language: 'zh', count: '10' });
    let data = await fetchJson(query(input), {}, 5000);
    if (!data.results?.length && suffixed) data = await fetchJson(query(suffixed), {}, 5000);
    return (data.results || []).map(place => ({
      id: place.id,
      name: place.name,
      latitude: place.latitude,
      longitude: place.longitude,
      detail: [...new Set([place.admin2, place.admin1, place.country].filter(x => x && x !== place.name))].join(' · '),
    }));
  }

  function nameBitmap(name) {
    const canvas = document.createElement('canvas');
    canvas.width = 192;
    canvas.height = 32;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.font = 'bold 27px "Microsoft YaHei", "PingFang SC", sans-serif';
    ctx.fillStyle = '#000';
    ctx.textBaseline = 'middle';
    ctx.fillText(name, 0, 16, canvas.width);
    const rgba = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
    const bytes = new Uint8Array(canvas.width * canvas.height / 8);
    for (let i = 0; i < canvas.width * canvas.height; ++i) {
      if (rgba[4 * i] < 128) bytes[i >> 3] |= 128 >> (i & 7);
    }
    return btoa(String.fromCharCode(...bytes));
  }

  window.HueLeafWeather = Object.freeze({ search, localSearch, nameBitmap });
})();
