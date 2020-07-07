/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include <osgEarth/TerrainTileModelFactory>
#include <osgEarth/ImageToHeightFieldConverter>
#include <osgEarth/Map>
#include <osgEarth/Registry>
#include <osgEarth/LandCoverLayer>
#include <osgEarth/Metrics>

#include <osg/Texture2D>
#include <osg/Texture2DArray>

#define LC "[TerrainTileModelFactory] "

using namespace osgEarth;

//.........................................................................

CreateTileManifest::CreateTileManifest()
{
    _includesElevation = false;
    _includesLandCover = false;
}

void CreateTileManifest::insert(const Layer* layer)
{
    if (layer)
    {
        _layers[layer->getUID()] = layer->getRevision();

        if (dynamic_cast<const ElevationLayer*>(layer))
            _includesElevation = true;

        if (dynamic_cast<const LandCoverLayer*>(layer))
            _includesLandCover = true;
    }
}

bool CreateTileManifest::excludes(const Layer* layer) const
{
    return !empty() && _layers.find(layer->getUID()) == _layers.end();
}

bool CreateTileManifest::empty() const
{
    return _layers.empty();
}

bool CreateTileManifest::inSyncWith(const Map* map) const
{
    for(LayerTable::const_iterator i = _layers.begin();
        i != _layers.end();
        ++i)
    {
        const Layer* layer = map->getLayerByUID(i->first);

        // note: if the layer is NULL, it was removed, so let it pass.

        if (layer != NULL && layer->getRevision() != i->second)
        {
            return false;
        }
    }
    return true;
}

void CreateTileManifest::updateRevisions(const Map* map)
{
    for(LayerTable::iterator i = _layers.begin();
        i != _layers.end();
        ++i)
    {
        const Layer* layer = map->getLayerByUID(i->first);
        if (layer)
        {
            i->second = layer->getRevision();
        }
    }
}

bool CreateTileManifest::includes(const Layer* layer) const
{
    return includes(layer->getUID());
}

bool CreateTileManifest::includes(UID uid) const
{
    return empty() || _layers.find(uid) != _layers.end();
}

bool CreateTileManifest::includesElevation() const
{
    return empty() || _includesElevation;
}

bool CreateTileManifest::includesLandCover() const
{
    return empty() || _includesLandCover;
}

//.........................................................................

TerrainTileModelFactory::TerrainTileModelFactory(const TerrainOptions& options) :
_options( options )
{
    // Create an empty texture that we can use as a placeholder
    _emptyColorTexture = new osg::Texture2D(ImageUtils::createEmptyImage());
    _emptyColorTexture->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());

    osg::Image* landCoverImage = LandCover::createImage(1u);
    ImageUtils::PixelWriter writeLC(landCoverImage);
    writeLC(osg::Vec4(0,0,0,0), 0, 0);
    _emptyLandCoverTexture = new osg::Texture2D(landCoverImage);
    _emptyLandCoverTexture->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());
}

TerrainTileModel*
TerrainTileModelFactory::createTileModel(
    const Map*                       map,
    const TileKey&                   key,
    const CreateTileManifest&        manifest,
    const TerrainEngineRequirements* requirements,
    ProgressCallback*                progress)
{
    OE_PROFILING_ZONE;
    // Make a new model:
    osg::ref_ptr<TerrainTileModel> model = new TerrainTileModel(
        key,
        map->getDataModelRevision() );

    // assemble all the components:
    addColorLayers(model.get(), map, requirements, key, manifest, progress, false);

    if ( requirements == 0L || requirements->elevationTexturesRequired() )
    {
        unsigned border = (requirements && requirements->elevationBorderRequired()) ? 1u : 0u;

        addElevation( model.get(), map, key, manifest, border, progress );
    }

    addLandCover(model.get(), map, key, requirements, manifest, progress);

    // done.
    return model.release();
}

TerrainTileModel*
TerrainTileModelFactory::createStandaloneTileModel(
    const Map*                       map,
    const TileKey&                   key,
    const CreateTileManifest&        manifest,
    const TerrainEngineRequirements* requirements,
    ProgressCallback*                progress)
{
    OE_PROFILING_ZONE;
    // Make a new model:
    osg::ref_ptr<TerrainTileModel> model = new TerrainTileModel(
        key,
        map->getDataModelRevision());

    // assemble all the components:
    addColorLayers(model.get(), map, requirements, key, manifest, progress, true);

    if (requirements == 0L || requirements->elevationTexturesRequired())
    {
        unsigned border = (requirements && requirements->elevationBorderRequired()) ? 1u : 0u;

        addStandaloneElevation(model.get(), map, key, manifest, border, progress);
    }

    addStandaloneLandCover(model.get(), map, key, requirements, manifest, progress);

    //addPatchLayers(model.get(), map, key, filter, progress, true);

    // done.
    return model.release();
}

TerrainTileImageLayerModel*
TerrainTileModelFactory::addImageLayer(
    TerrainTileModel* model,
    ImageLayer* imageLayer,
    const TileKey& key,
    const TerrainEngineRequirements* reqs,
    ProgressCallback* progress)
{
    TerrainTileImageLayerModel* layerModel = NULL;
    osg::Texture* tex = 0L;
    TextureWindow window;
    osg::Matrix scaleBiasMatrix;
        
    if (imageLayer->isKeyInLegalRange(key) && imageLayer->mayHaveData(key))
    {
        if (imageLayer->useCreateTexture())
        {
            window = imageLayer->createTexture(key, progress);
            tex = window.getTexture();
            scaleBiasMatrix = window.getMatrix();
        }

        else
        {
            GeoImage geoImage = imageLayer->createImage(key, progress);

            if (geoImage.valid())
            {
                if (imageLayer->isCoverage())
                    tex = createCoverageTexture(geoImage.getImage());
                else
                    tex = createImageTexture(geoImage.getImage(), imageLayer);
            }
        }
    }

    // if this is the first LOD, and the engine requires that the first LOD
    // be populated, make an empty texture if we didn't get one.
    if (tex == 0L &&
        _options.firstLOD() == key.getLOD() &&
        reqs && reqs->fullDataAtFirstLodRequired())
    {
        tex = _emptyColorTexture.get();
    }

    if (tex)
    {
        tex->setName(model->getKey().str());

        layerModel = new TerrainTileImageLayerModel();

        layerModel->setImageLayer(imageLayer);

        layerModel->setTexture(tex);
        layerModel->setMatrix(new osg::RefMatrixf(scaleBiasMatrix));
        layerModel->setRevision(imageLayer->getRevision());

        model->colorLayers().push_back(layerModel);

        if (imageLayer->isShared())
        {
            model->sharedLayers().push_back(layerModel);
        }

        if (imageLayer->isDynamic())
        {
            model->setRequiresUpdateTraverse(true);
        }
    }

    return layerModel;
}

void
TerrainTileModelFactory::addStandaloneImageLayer(
    TerrainTileModel* model,
    ImageLayer* imageLayer,
    const TileKey& key,
    const TerrainEngineRequirements* reqs,
    ProgressCallback* progress)
{
    TerrainTileImageLayerModel* layerModel = NULL;
    TileKey keyToUse = key;
    osg::Matrixf scaleBiasMatrix;
    while (keyToUse.valid() && !layerModel)
    {
        layerModel = addImageLayer(model, imageLayer, keyToUse, reqs, progress);
        if (!layerModel)
        {
            TileKey parentKey = keyToUse.createParentKey();
            if (parentKey.valid())
            {
                osg::Matrixf sb;
                keyToUse.getExtent().createScaleBias(parentKey.getExtent(), sb);
                scaleBiasMatrix.postMult(sb);
            }
            keyToUse = parentKey;
        }
    }
    if (layerModel)
    {
        layerModel->setMatrix(new osg::RefMatrixf(scaleBiasMatrix));
    }
}

void
TerrainTileModelFactory::addColorLayers(
    TerrainTileModel* model,
    const Map* map,
    const TerrainEngineRequirements* reqs,
    const TileKey& key,
    const CreateTileManifest& manifest,
    ProgressCallback* progress,
    bool standalone)
{
    OE_PROFILING_ZONE;

    int order = 0;

    LayerVector layers;
    map->getLayers(layers);

    for (LayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
    {
        Layer* layer = i->get();

        if (!layer->isOpen())
            continue;

        if (layer->getRenderType() != layer->RENDERTYPE_TERRAIN_SURFACE)
            continue;

        if (manifest.excludes(layer))
            continue;

        ImageLayer* imageLayer = dynamic_cast<ImageLayer*>(layer);
        if (imageLayer)
        {
            if (standalone)
            {
                addStandaloneImageLayer(model, imageLayer, key, reqs, progress);
            }
            else
            {
                addImageLayer(model, imageLayer, key, reqs, progress);
            }
        }
        else // non-image kind of TILE layer:
        {
            TerrainTileColorLayerModel* colorModel = new TerrainTileColorLayerModel();
            colorModel->setLayer(layer);
            colorModel->setRevision(layer->getRevision());
            model->colorLayers().push_back(colorModel);
        }
    }
}

#if 0
void
TerrainTileModelFactory::addPatchLayers(
    TerrainTileModel* model,
    const Map* map,
    const TileKey&    key,
    const CreateTileManifest& manifest,
    ProgressCallback* progress,
    bool fallback)
{
    PatchLayerVector patchLayers;
    map->getLayers(patchLayers);

    for(PatchLayerVector::const_iterator i = patchLayers.begin();
        i != patchLayers.end();
        ++i )
    {
        PatchLayer* layer = i->get();

        if (!layer->isOpen())
            continue;

        if (manifest.excludes(layer))
            continue;

        if (layer->getAcceptCallback() == 0L || layer->getAcceptCallback()->acceptKey(key))
        {
            GeoNode node = layer->createNode(key, progress);
            if (node.valid())
            {
                TerrainTilePatchLayerModel* patchModel = new TerrainTilePatchLayerModel();
                patchModel->setPatchLayer(layer);
                patchModel->setRevision(layer->getRevision());
                patchModel->setNode(node.getNode());
            }
        }
    }
}
#endif

void
TerrainTileModelFactory::addElevation(
    TerrainTileModel*            model,
    const Map*                   map,
    const TileKey&               key,
    const CreateTileManifest&    manifest,
    unsigned                     border,
    ProgressCallback*            progress)
{
    bool needElevation = manifest.includesElevation();
    ElevationLayerVector layers;
    map->getLayers(layers);
    int combinedRevision = map->getDataModelRevision();

    if (!manifest.empty())
    {
        for(ElevationLayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
        {
            const ElevationLayer* layer = i->get();

            if (needElevation == false && !manifest.excludes(layer))
            {
                needElevation = true;
            }

            combinedRevision += layer->getRevision();
        }
    }
    if (!needElevation)
        return;

    osg::ref_ptr<ElevationTexture> elevTex;

    bool getNormalMap = (_options.normalMaps() == true);

    const bool acceptLowerRes = false;

    if (map->getElevationPool()->getTile(key, acceptLowerRes, elevTex, NULL, progress))
    {
        osg::ref_ptr<TerrainTileElevationModel> layerModel = new TerrainTileElevationModel();

        layerModel->setRevision(combinedRevision);

        if ( elevTex.valid() )
        {
            // Make a normal map
            NormalMapGenerator gen;

            Distance resolution(
                key.getExtent().height() / (osgEarth::ELEVATION_TILE_SIZE-1),
                key.getProfile()->getSRS()->getUnits());
            
            osg::Texture2D* normalMap = gen.createNormalMap(key, map, &_workingSet, progress);

            if (normalMap)
            {
                elevTex->setNormalMapTexture(normalMap);
            }

            // Made an image, so store this as a texture with no matrix.
            layerModel->setTexture( elevTex.get() );

            // Keep the heightfield pointer around for legacy 3rd party usage (VRF)
            layerModel->setHeightField(elevTex->getHeightField());

            model->elevationModel() = layerModel.get();
        }
    }
}

void
TerrainTileModelFactory::addStandaloneElevation(
    TerrainTileModel*            model,
    const Map*                   map,
    const TileKey&               key,
    const CreateTileManifest&    manifest,
    unsigned                     border,
    ProgressCallback*            progress)
{
    TileKey keyToUse = key;
    while (keyToUse.valid() && model->elevationModel().valid() == false)
    {
        addElevation(model, map, keyToUse, manifest, border, progress);
        if (model->elevationModel() == NULL)
        {
            keyToUse = keyToUse.createParentKey();
        }
    }
    if (model->elevationModel().valid())
    {
        osg::Matrixf scaleBiasMatrix;
        key.getExtent().createScaleBias(keyToUse.getExtent(), scaleBiasMatrix);
        model->elevationModel()->setMatrix(new osg::RefMatrixf(scaleBiasMatrix));
    }
}

TerrainTileLandCoverModel*
TerrainTileModelFactory::addLandCover(
    TerrainTileModel*            model,
    const Map*                   map,
    const TileKey&               key,
    const TerrainEngineRequirements* reqs,
    const CreateTileManifest&    manifest,
    ProgressCallback*            progress)
{
    TerrainTileLandCoverModel* landCoverModel = NULL;

    // Note. We only support one land cover layer...
    LandCoverLayerVector layers;
    map->getLayers(layers);
    int combinedRevision = map->getDataModelRevision();

    // any land cover layer means using them all:
    bool needLandCover = manifest.includesLandCover();

    if (!manifest.empty())
    {
        for(LandCoverLayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
        {
            const LandCoverLayer* layer = i->get();
            if (layer->isOpen())
            {
                if (needLandCover == false && !manifest.excludes(layer))
                {
                    needLandCover = true;
                }

                combinedRevision += layer->getRevision();
            }
        }
    }

    if (!needLandCover)
    {
        return NULL;
    }

    osg::ref_ptr<osg::Image> coverageImage;

    osg::ref_ptr<osg::Texture> tex;

    if (layers.populateLandCoverImage(coverageImage, key, progress))
    {
        tex = createCoverageTexture(coverageImage.get());
    }

    // if this is the first LOD, and the engine requires that the first LOD
    // be populated, make an empty texture if we didn't get one.
    if (tex == 0L &&
        _options.firstLOD() == key.getLOD() &&
        reqs && reqs->fullDataAtFirstLodRequired())
    {
        tex = _emptyLandCoverTexture.get();
    }

    if (tex)
    {
        tex->setName(model->getKey().str());

        landCoverModel = new TerrainTileLandCoverModel();
        landCoverModel->setRevision(combinedRevision);

        landCoverModel->setTexture(tex.get());

        model->landCoverModel() = landCoverModel;
    }

    return landCoverModel;
}

void
TerrainTileModelFactory::addStandaloneLandCover(
    TerrainTileModel*            model,
    const Map*                   map,
    const TileKey&               key,
    const TerrainEngineRequirements* reqs,
    const CreateTileManifest&    manifest,
    ProgressCallback*            progress)
{
    TerrainTileLandCoverModel* layerModel = NULL;
    TileKey keyToUse = key;
    osg::Matrixf scaleBiasMatrix;
    while (keyToUse.valid() && !layerModel)
    {
        layerModel = addLandCover(model, map, keyToUse, reqs, manifest, progress);
        if (!layerModel)
        {
            TileKey parentKey = keyToUse.createParentKey();
            if (parentKey.valid())
            {
                osg::Matrixf sb;
                keyToUse.getExtent().createScaleBias(parentKey.getExtent(), sb);
                scaleBiasMatrix.postMult(sb);
            }
            keyToUse = parentKey;
        }
    }
    if (layerModel)
    {
        layerModel->setMatrix(new osg::RefMatrixf(scaleBiasMatrix));
    }
}

osg::Texture*
TerrainTileModelFactory::createImageTexture(const osg::Image* image,
                                            const ImageLayer* layer) const
{
    osg::Texture* tex = nullptr;
    bool hasMipMaps = false;

    if (image->r() == 1)
    {
        const osg::Image* compressed = ImageUtils::compressImage(image, layer->getCompressionMethod());
        const osg::Image* mipmapped = ImageUtils::mipmapImage(compressed);
        tex = new osg::Texture2D(const_cast<osg::Image*>(mipmapped));
        hasMipMaps = mipmapped->isMipmap();

        if (layer->getCompressionMethod() == "gpu" && !mipmapped->isCompressed())
            tex->setInternalFormatMode(tex->USE_S3TC_DXT5_COMPRESSION);
    }

    else // if (image->r() > 1)
    {
        std::vector< osg::ref_ptr<const osg::Image> > images;
        ImageUtils::flattenImage(image, images);
        
        const osg::Image* compressed;
        for(auto& ref : images)
        {
            compressed = ImageUtils::compressImage(image, layer->getCompressionMethod());
            ref = ImageUtils::mipmapImage(compressed);

            if (layer->getCompressionMethod() == "gpu" && !compressed->isCompressed())
                tex->setInternalFormatMode(tex->USE_S3TC_DXT5_COMPRESSION);

            hasMipMaps = compressed->isMipmap();
        }

        osg::Texture2DArray* tex2dArray = new osg::Texture2DArray();

        tex2dArray->setTextureDepth(images.size());
        tex2dArray->setInternalFormat(images[0]->getInternalTextureFormat());
        tex2dArray->setSourceFormat(images[0]->getPixelFormat());
        for (int i = 0; i < (int)images.size(); ++i)
            tex2dArray->setImage(i, const_cast<osg::Image*>(images[i].get()));

        tex = tex2dArray;
    }

    tex->setDataVariance(osg::Object::STATIC);
    tex->setWrap( osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE );
    tex->setWrap( osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE );
    tex->setResizeNonPowerOfTwoHint(false);

    osg::Texture::FilterMode magFilter =
        layer ? layer->options().magFilter().get() : osg::Texture::LINEAR;
    osg::Texture::FilterMode minFilter =
        layer ? layer->options().minFilter().get() : osg::Texture::LINEAR;

    tex->setFilter( osg::Texture::MAG_FILTER, magFilter );
    tex->setFilter( osg::Texture::MIN_FILTER, minFilter );
    tex->setMaxAnisotropy( 4.0f );

    // Disable mip mapping if we don't have it
    if (!hasMipMaps)
    {
        tex->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR );
    }

    tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());

    // For GL_RED, swizzle the RGBA all to RED in order to match old GL_LUMINANCE behavior
    for(unsigned i=0; i<tex->getNumImages(); ++i)
    {
        if (tex->getImage(i) && tex->getImage(i)->getPixelFormat() == GL_RED)
        {
            tex->setSwizzle(osg::Vec4i(GL_RED, GL_RED, GL_RED, GL_RED));
            break;
        }
    }
    
    return tex;
}

osg::Texture*
TerrainTileModelFactory::createCoverageTexture(const osg::Image* image) const
{
    osg::Texture2D* tex = new osg::Texture2D(const_cast<osg::Image*>(image));
    tex->setDataVariance(osg::Object::STATIC);

    tex->setInternalFormat(LandCover::getTextureFormat());

    tex->setWrap( osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE );
    tex->setWrap( osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE );
    tex->setResizeNonPowerOfTwoHint(false);

    tex->setFilter( osg::Texture::MAG_FILTER, osg::Texture::NEAREST );
    tex->setFilter( osg::Texture::MIN_FILTER, osg::Texture::NEAREST );

    tex->setMaxAnisotropy( 1.0f );

    tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());

    return tex;
}

osg::Texture*
TerrainTileModelFactory::createElevationTexture(const osg::Image* image) const
{
    osg::Texture2D* tex = new osg::Texture2D(const_cast<osg::Image*>(image));
    tex->setDataVariance(osg::Object::STATIC);
    tex->setInternalFormat(GL_R32F);
    tex->setFilter( osg::Texture::MAG_FILTER, osg::Texture::LINEAR );
    tex->setFilter( osg::Texture::MIN_FILTER, osg::Texture::NEAREST );
    tex->setWrap  ( osg::Texture::WRAP_S,     osg::Texture::CLAMP_TO_EDGE );
    tex->setWrap  ( osg::Texture::WRAP_T,     osg::Texture::CLAMP_TO_EDGE );
    tex->setResizeNonPowerOfTwoHint( false );
    tex->setMaxAnisotropy( 1.0f );
    tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());
    return tex;
}
