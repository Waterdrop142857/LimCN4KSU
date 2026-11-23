import assert from 'node:assert/strict';
import { releaseAssetVersion, selectReleaseAsset } from './release.mjs';

const root = 'https://github.com/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/download/v1/';
assert.equal(releaseAssetVersion(`${root}LimbusLocalize_12.7z`), '12');
assert.equal(releaseAssetVersion('https://example.com/releases/download/v1/LimbusLocalize_12.7z'), '');
assert.equal(releaseAssetVersion(`${root}LimbusLocalize_12.7z?token=x`), '');
assert.equal(releaseAssetVersion(`${root}../LimbusLocalize_12.7z`), '');

const selected = selectReleaseAsset({ assets: [
  { name: 'LimbusLocalize_9.7z', browser_download_url: `${root}LimbusLocalize_9.7z` },
  { name: 'LimbusLocalize_12.7z', browser_download_url: `${root}LimbusLocalize_12.7z`,
    digest: `sha256:${'a'.repeat(64)}` },
  { name: 'LimbusLocalize_999.7z', browser_download_url: 'https://example.com/asset' },
] });
assert.equal(selected?.version, '12');
assert.equal(selected?.digest, 'a'.repeat(64));
assert.equal(selectReleaseAsset({ assets: [{ name: 'bad', browser_download_url: root }] }), null);
console.log('release selection passed');
