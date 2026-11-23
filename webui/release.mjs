const RELEASE_PREFIX =
  'https://github.com/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/download/';

export const RELEASE_API =
  'https://api.github.com/repos/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/latest';

export function releaseAssetVersion(url) {
  if (typeof url !== 'string' || !url.startsWith(RELEASE_PREFIX)) return '';
  const parts = url.slice(RELEASE_PREFIX.length).split('/');
  if (parts.length !== 2 || !/^[A-Za-z0-9._%+-]+$/.test(parts[0]) ||
      parts[0].includes('..')) return '';
  const match = /^LimbusLocalize_([0-9]+)\.7z$/.exec(parts[1]);
  return match ? match[1] : '';
}

export function selectReleaseAsset(release) {
  if (!release || !Array.isArray(release.assets)) return null;
  let best = null;
  for (const asset of release.assets) {
    if (!asset || typeof asset.name !== 'string') continue;
    const url = asset.browser_download_url;
    const version = releaseAssetVersion(url);
    if (!version || asset.name !== `LimbusLocalize_${version}.7z`) continue;
    let digest = '';
    if (asset.digest != null && asset.digest !== '') {
      if (typeof asset.digest !== 'string') continue;
      const match = /^sha256:([0-9a-f]{64})$/.exec(asset.digest);
      if (!match) continue;
      digest = match[1];
    }
    if (best && (version.length < best.version.length ||
        (version.length === best.version.length && version <= best.version))) continue;
    best = { name: asset.name, url, version, digest };
  }
  return best;
}
