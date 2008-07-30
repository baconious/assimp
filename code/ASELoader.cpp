/*
---------------------------------------------------------------------------
Open Asset Import Library (ASSIMP)
---------------------------------------------------------------------------

Copyright (c) 2006-2008, ASSIMP Development Team

All rights reserved.

Redistribution and use of this software in source and binary forms, 
with or without modification, are permitted provided that the following 
conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the ASSIMP team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the ASSIMP Development Team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

/** @file Implementation of the ASE importer class */

// internal headers
#include "ASELoader.h"
#include "3DSSpatialSort.h"
#include "MaterialSystem.h"
#include "StringComparison.h"
#include "TextureTransform.h"

// utilities
#include "fast_atof.h"
#include "qnan.h"

// ASSIMP public headers
#include "../include/IOStream.h"
#include "../include/IOSystem.h"
#include "../include/aiMesh.h"
#include "../include/aiScene.h"
#include "../include/aiAssert.h"
#include "../include/DefaultLogger.h"

#include <boost/scoped_ptr.hpp>

using namespace Assimp;
using namespace Assimp::ASE;

// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
ASEImporter::ASEImporter()
{
}
// ------------------------------------------------------------------------------------------------
// Destructor, private as well 
ASEImporter::~ASEImporter()
{
}
// ------------------------------------------------------------------------------------------------
// Returns whether the class can handle the format of the given file. 
bool ASEImporter::CanRead( const std::string& pFile, IOSystem* pIOHandler) const
{
	// simple check of file extension is enough for the moment
	std::string::size_type pos = pFile.find_last_of('.');
	// no file extension - can't read
	if( pos == std::string::npos)
		return false;
	std::string extension = pFile.substr( pos);

	if (extension.length() < 4)return false;
	if (extension[0] != '.')return false;

	if (extension[1] != 'a' && extension[1] != 'A')return false;
	if (extension[2] != 's' && extension[2] != 'S')return false;

	// NOTE: Sometimes the extension .ASK is also used
	// however, often it only contains static animation skeletons
	// without the real animations.
	if (extension[3] != 'e' && extension[3] != 'E' &&
		extension[3] != 'k' && extension[3] != 'K')return false;

	return true;
}
// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure. 
void ASEImporter::InternReadFile( 
	const std::string& pFile, aiScene* pScene, IOSystem* pIOHandler)
{
	boost::scoped_ptr<IOStream> file( pIOHandler->Open( pFile, "rb"));

	// Check whether we can read from the file
	if( file.get() == NULL)
	{
		throw new ImportErrorException( "Failed to open ASE file " + pFile + ".");
	}

	size_t fileSize = file->FileSize();

	std::string::size_type pos = pFile.find_last_of('.');
	std::string extension = pFile.substr( pos);
	if(extension[3] == 'k' || extension[3] == 'K')
	{
		this->mIsAsk = true;
	}
	else this->mIsAsk = false;

	// allocate storage and copy the contents of the file to a memory buffer
	// (terminate it with zero)
	this->mBuffer = new unsigned char[fileSize+1];
	this->pcScene = pScene;
	file->Read( (void*)mBuffer, 1, fileSize);
	this->mBuffer[fileSize] = '\0';

	// construct an ASE parser and parse the file
	this->mParser = new ASE::Parser((const char*)this->mBuffer);
	this->mParser->Parse();

	// if absolutely no material has been loaded from the file
	// we need to generate a default material
	this->GenerateDefaultMaterial();

	// process all meshes
	std::vector<aiMesh*> avOutMeshes;
	avOutMeshes.reserve(this->mParser->m_vMeshes.size()*2);
	for (std::vector<ASE::Mesh>::iterator
		i =  this->mParser->m_vMeshes.begin();
		i != this->mParser->m_vMeshes.end();++i)
	{
		if ((*i).bSkip)continue;

		// transform all vertices into worldspace
		// world2obj transform is specified in the
		// transformation matrix of a scenegraph node
		this->TransformVertices(*i);

		// now we need to create proper meshes from the import
		// we need to split them by materials, build valid vertex/face lists ...
		this->BuildUniqueRepresentation(*i);

		// need to generate proper vertex normals if necessary
		this->GenerateNormals(*i);

		// convert all meshes to aiMesh objects
		this->ConvertMeshes(*i,avOutMeshes);
	}
	
	// now build the output mesh list. remove dummies
	pScene->mNumMeshes = (unsigned int)avOutMeshes.size();
	aiMesh** pp = pScene->mMeshes = new aiMesh*[pScene->mNumMeshes];
	for (std::vector<aiMesh*>::const_iterator
		i =  avOutMeshes.begin();
		i != avOutMeshes.end();++i)
	{
		if (!(*i)->mNumFaces)continue;
		*pp++ = *i;
	}
	pScene->mNumMeshes = (unsigned int)(pp - pScene->mMeshes);

	// buil final material indices (remove submaterials and make the final list)
	this->BuildMaterialIndices();

	// build the final node graph
	this->BuildNodes();

	// build output animations
	this->BuildAnimations();

	// delete the ASE parser
	delete this->mParser;
	this->mParser = NULL;
	return;
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::GenerateDefaultMaterial()
{
	ai_assert(NULL != this->mParser);

	bool bHas = false;
	for (std::vector<ASE::Mesh>::iterator
		i =  this->mParser->m_vMeshes.begin();
		i != this->mParser->m_vMeshes.end();++i)
	{
		if ((*i).bSkip)continue;
		if (ASE::Face::DEFAULT_MATINDEX == (*i).iMaterialIndex)
		{
			(*i).iMaterialIndex = (unsigned int)this->mParser->m_vMaterials.size();
			bHas = true;
		}
	}
	if (bHas || this->mParser->m_vMaterials.empty())
	{
		// add a simple material without sub materials to the parser's list
		this->mParser->m_vMaterials.push_back ( ASE::Material() );
		ASE::Material& mat = this->mParser->m_vMaterials.back();

		mat.mDiffuse = aiColor3D(0.5f,0.5f,0.5f);
		mat.mSpecular = aiColor3D(1.0f,1.0f,1.0f);
		mat.mAmbient = aiColor3D(0.05f,0.05f,0.05f);
		mat.mShading = Dot3DSFile::Gouraud;
		mat.mName = AI_DEFAULT_MATERIAL_NAME;
	}
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::BuildAnimations()
{
	// check whether we have at least one mesh which has animations
	std::vector<ASE::Mesh>::iterator i =  this->mParser->m_vMeshes.begin();
	unsigned int iNum = 0;
	for (;i != this->mParser->m_vMeshes.end();++i)
	{
		if ((*i).bSkip)continue;
		if ((*i).mAnim.akeyPositions.size() > 1 || (*i).mAnim.akeyRotations.size() > 1)
			++iNum;
	}
	if (iNum)
	{
		this->pcScene->mNumAnimations = 1;
		this->pcScene->mAnimations = new aiAnimation*[1];
		aiAnimation* pcAnim = this->pcScene->mAnimations[0] = new aiAnimation();
		pcAnim->mNumBones = iNum;
		pcAnim->mBones = new aiBoneAnim*[iNum];
		pcAnim->mTicksPerSecond = this->mParser->iFrameSpeed * this->mParser->iTicksPerFrame;

		iNum = 0;
		i =  this->mParser->m_vMeshes.begin();
		for (;i != this->mParser->m_vMeshes.end();++i)
		{
			if ((*i).bSkip)continue;
			if ((*i).mAnim.akeyPositions.size() > 1 || (*i).mAnim.akeyRotations.size() > 1)
			{
				aiBoneAnim* pcBoneAnim = pcAnim->mBones[iNum++] = new aiBoneAnim();
				pcBoneAnim->mBoneName.Set((*i).mName);

				// copy position keys
				if ((*i).mAnim.akeyPositions.size() > 1 )
				{
					pcBoneAnim->mNumPositionKeys = (unsigned int) (*i).mAnim.akeyPositions.size();
					pcBoneAnim->mPositionKeys = new aiVectorKey[pcBoneAnim->mNumPositionKeys];

					::memcpy(pcBoneAnim->mPositionKeys,&(*i).mAnim.akeyPositions[0],
						pcBoneAnim->mNumPositionKeys * sizeof(aiVectorKey));

					for (unsigned int qq = 0; qq < pcBoneAnim->mNumPositionKeys;++qq)
					{
						double dTime = pcBoneAnim->mPositionKeys[qq].mTime;
						pcAnim->mDuration = std::max(pcAnim->mDuration,dTime);
					}
				}
				// copy rotation keys
				if ((*i).mAnim.akeyRotations.size() > 1 )
				{
					pcBoneAnim->mNumRotationKeys = (unsigned int) (*i).mAnim.akeyPositions.size();
					pcBoneAnim->mRotationKeys = new aiQuatKey[pcBoneAnim->mNumPositionKeys];

					::memcpy(pcBoneAnim->mRotationKeys,&(*i).mAnim.akeyRotations[0],
						pcBoneAnim->mNumRotationKeys * sizeof(aiQuatKey));

					for (unsigned int qq = 0; qq < pcBoneAnim->mNumRotationKeys;++qq)
					{
						double dTime = pcBoneAnim->mRotationKeys[qq].mTime;
						pcAnim->mDuration = std::max(pcAnim->mDuration,dTime);
					}
				}
			}
		}
	}
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::AddNodes(aiNode* pcParent,const char* szName)
{
	aiMatrix4x4 m;
	ASE::DecompTransform dec(m);
	this->AddNodes(pcParent,szName,dec);
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::AddNodes(aiNode* pcParent,const char* szName,
	const ASE::DecompTransform& decompTrafo)
{
	const size_t len = szName ? strlen(szName) : 0;

	ai_assert(4 <= AI_MAX_NUMBER_OF_COLOR_SETS);
	std::vector<aiNode*> apcNodes;
	aiMesh** pcMeshes = pcScene->mMeshes;
	for (unsigned int i = 0; i < pcScene->mNumMeshes;++i)
	{
		// get the name of the mesh
		aiMesh* pcMesh = *pcMeshes++;
		const ASE::Mesh& mesh = *((const ASE::Mesh*)pcMesh->mColors[2]);

		// TODO: experimental quick'n'dirty, clean this up ...
		std::string szMyName[2] = {mesh.mName,mesh.mParent} ;
		if (!szMyName)
		{
			continue;
		}
		if (szName)
		{
			if( len != szMyName[1].length() ||
				0 != ASSIMP_stricmp ( szName, szMyName[1].c_str() ))
			{
				continue;
			}
		} 
		else if ('\0' != szMyName[1].c_str()[0])continue;

		apcNodes.push_back(new aiNode());
		aiNode* node = apcNodes.back();

		node->mName.Set(szMyName[0]);
		node->mNumMeshes = 1;
		node->mMeshes = new unsigned int[1];
		node->mMeshes[0] = i;
		node->mParent = pcParent;

		aiMatrix4x4 mParentAdjust  = decompTrafo.mMatrix;
		mParentAdjust.Inverse();
		//if(ComputeLocalToWorldShift(mParentAdjust, decompTrafo, mesh.inherit))
		{
			node->mTransformation = mParentAdjust*mesh.mTransform;
		}
		//else node->mTransformation = mesh.mTransform;
		
		// Transform all vertices of the mesh back into their local space -> 
		// at the moment they are pretransformed
		aiMatrix4x4 mInverse = mesh.mTransform;
		mInverse.Inverse();

		aiVector3D* pvCurPtr = pcMesh->mVertices;
		const aiVector3D* const pvEndPtr = pcMesh->mVertices + pcMesh->mNumVertices;
		while (pvCurPtr != pvEndPtr)
		{
			*pvCurPtr = mInverse * (*pvCurPtr);
			pvCurPtr++;
		}

		//pcMesh->mColors[2] = NULL;

		// add sub nodes
		aiMatrix4x4 mNewAbs = decompTrafo.mMatrix * node->mTransformation;
		ASE::DecompTransform dec( mNewAbs);
		this->AddNodes(node,node->mName.data,dec);
	}

	// allocate enough space for the child nodes
	pcParent->mNumChildren = (unsigned int)apcNodes.size();
	pcParent->mChildren = new aiNode*[apcNodes.size()];

	// now build all nodes
	for (unsigned int p = 0; p < apcNodes.size();++p)
	{
		pcParent->mChildren[p] = apcNodes[p];
	}
	return;
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::BuildNodes()
{
	ai_assert(NULL != pcScene);

	// allocate the root node
	pcScene->mRootNode = new aiNode();
	pcScene->mRootNode->mNumMeshes = 0;
	pcScene->mRootNode->mMeshes = 0;
	pcScene->mRootNode->mName.Set("<root>");

	// add all nodes
	this->AddNodes(pcScene->mRootNode,NULL);

	// now iterate through al meshes and find those that have not yet
	// been added to the nodegraph (= their parent could not be recognized)
	std::vector<unsigned int> aiList;
	for (unsigned int i = 0; i < pcScene->mNumMeshes;++i)
	{
		// get the name of the mesh
		const ASE::Mesh& mesh = *((const ASE::Mesh*)pcScene->mMeshes[i]->mColors[2]);
		// TODO: experimental quick'n'dirty, clean this up ...
		std::string szMyName[2] = {mesh.mName,mesh.mParent} ;

		if (!szMyName)
		{
			continue;
		}

		// check whether our parent is known
		bool bKnowParent = false;
		for (unsigned int i2 = 0; i2 < pcScene->mNumMeshes;++i2)
		{
			if (i2 == i)continue;
			const ASE::Mesh& mesh2 = *((const ASE::Mesh*)pcScene->mMeshes[i2]->mColors[2]);
			// TODO: experimental quick'n'dirty, clean this up ...
			std::string szMyName2[2] = {mesh2.mName,mesh2.mParent} ;
			if (!szMyName2)
			{
				continue;
			}
			if (szMyName[0].length() == szMyName2[1].length() &&
				0 == ASSIMP_stricmp ( szMyName[1].c_str(), szMyName2[0].c_str()))
			{
				bKnowParent = true;
				break;
			}
		}
		if (!bKnowParent)
		{
			aiList.push_back(i);
		}
	}
	if (!aiList.empty())
	{
		std::vector<aiNode*> apcNodes;
		apcNodes.reserve(aiList.size() + pcScene->mRootNode->mNumChildren);

		for (unsigned int i = 0; i < pcScene->mRootNode->mNumChildren;++i)
			apcNodes.push_back(pcScene->mRootNode->mChildren[i]);

		delete[] pcScene->mRootNode->mChildren;
		for (std::vector<unsigned int>::const_iterator
			i =  aiList.begin();
			i != aiList.end();++i)
		{
			std::string* szMyName = (std::string*)pcScene->mMeshes[*i]->mColors[1];
			if (!szMyName)continue;

			// the parent is not known, so we can assume that we must add 
			// this node to the root node of the whole scene
			aiNode* pcNode = new aiNode();
			pcNode->mParent = pcScene->mRootNode;
			pcNode->mName.Set(szMyName[1]);
			this->AddNodes(pcNode,szMyName[1].c_str());
			apcNodes.push_back(pcNode);
		}
		pcScene->mRootNode->mChildren = new aiNode*[apcNodes.size()];
		for (unsigned int i = 0; i < apcNodes.size();++i)
			pcScene->mRootNode->mChildren[i] = apcNodes[i];

		pcScene->mRootNode->mNumChildren = (unsigned int)apcNodes.size();
	}

	for (unsigned int i = 0; i < pcScene->mNumMeshes;++i)
		pcScene->mMeshes[i]->mColors[2] = NULL;

	// if there is only one subnode, set it as root node
	if (1 == pcScene->mRootNode->mNumChildren)
	{
		aiNode* pc = pcScene->mRootNode;
		pcScene->mRootNode = pcScene->mRootNode->mChildren[0];
		pcScene->mRootNode->mParent = NULL;

		// make sure the destructor won't delete us ...
		delete[] pc->mChildren;
		pc->mChildren = NULL;
		pc->mNumChildren = 0;
		delete pc;
	}
	else if (0 == pcScene->mRootNode->mNumChildren)
	{
		throw new ImportErrorException("No nodes loaded. The ASE/ASK file is either empty or corrupt");
	}
	return;
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::TransformVertices(ASE::Mesh& mesh)
{
	// the matrix data is stored in column-major format,
	// but we need row major
	mesh.mTransform.Transpose();
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::BuildUniqueRepresentation(ASE::Mesh& mesh)
{
	// allocate output storage
	std::vector<aiVector3D> mPositions;
	std::vector<aiVector3D> amTexCoords[AI_MAX_NUMBER_OF_TEXTURECOORDS];
	std::vector<aiColor4D> mVertexColors;
	std::vector<aiVector3D> mNormals;
	std::vector<BoneVertex> mBoneVertices;

	unsigned int iSize = (unsigned int)mesh.mFaces.size() * 3;
	mPositions.resize(iSize);

	// optional texture coordinates
	for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS;++i)
	{
		if (!mesh.amTexCoords[i].empty())
		{
			amTexCoords[i].resize(iSize);
		}
	}
	// optional vertex colors
	if (!mesh.mVertexColors.empty())
	{
		mVertexColors.resize(iSize);
	}

	// optional vertex normals (vertex normals can simply be copied)
	if (!mesh.mNormals.empty())
	{
		mNormals.resize(iSize);
	}
	// bone vertices. There is no need to change the bone list
	if (!mesh.mBoneVertices.empty())
	{
		mBoneVertices.resize(iSize);
	}

	// iterate through all faces in the mesh
	unsigned int iCurrent = 0;
	for (std::vector<ASE::Face>::iterator
		i =  mesh.mFaces.begin();
		i != mesh.mFaces.end();++i)
	{
		for (unsigned int n = 0; n < 3;++n,++iCurrent)
		{
			mPositions[iCurrent] = mesh.mPositions[(*i).mIndices[n]];

			// add texture coordinates
			for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS;++c)
			{
				if (!mesh.amTexCoords[c].empty())
				{
					amTexCoords[c][iCurrent] = mesh.amTexCoords[c][(*i).amUVIndices[c][n]];
				}
			}
			// add vertex colors
			if (!mesh.mVertexColors.empty())
			{
				mVertexColors[iCurrent] = mesh.mVertexColors[(*i).mColorIndices[n]];
			}
			// add normal vectors
			if (!mesh.mNormals.empty())
			{
				mNormals[iCurrent] = mesh.mNormals[(*i).mIndices[n]];
			}

			// handle bone vertices
			if ((*i).mIndices[n] < mesh.mBoneVertices.size())
			{
				// (sometimes this will cause bone verts to be duplicated
				//  however, I' quite sure Schrompf' JoinVerticesStep
				//  will fix that again ...)
				mBoneVertices[iCurrent] =  mesh.mBoneVertices[(*i).mIndices[n]];
			}
		}
		// we need to flip the order of the indices
		(*i).mIndices[0] = iCurrent-1;
		(*i).mIndices[1] = iCurrent-2;
		(*i).mIndices[2] = iCurrent-3;
	}

	// replace the old arrays
	mesh.mNormals = mNormals;
	mesh.mPositions = mPositions;
	mesh.mVertexColors = mVertexColors;

	for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS;++c)
		mesh.amTexCoords[c] = amTexCoords[c];

	// now need to transform all vertices with the inverse of their
	// transformation matrix ...
	//aiMatrix4x4 mInverse = mesh.mTransform;
	//mInverse.Inverse();

	//for (std::vector<aiVector3D>::iterator
	//	i =  mesh.mPositions.begin();
	//	i != mesh.mPositions.end();++i)
	//{
	//	(*i) = mInverse * (*i);
	//}

	return;
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::ConvertMaterial(ASE::Material& mat)
{
	// allocate the output material
	mat.pcInstance = new MaterialHelper();

	// At first add the base ambient color of the
	// scene to	the material
	mat.mAmbient.r += this->mParser->m_clrAmbient.r;
	mat.mAmbient.g += this->mParser->m_clrAmbient.g;
	mat.mAmbient.b += this->mParser->m_clrAmbient.b;

	aiString name;
	name.Set( mat.mName);
	mat.pcInstance->AddProperty( &name, AI_MATKEY_NAME);

	// material colors
	mat.pcInstance->AddProperty( &mat.mAmbient, 1, AI_MATKEY_COLOR_AMBIENT);
	mat.pcInstance->AddProperty( &mat.mDiffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
	mat.pcInstance->AddProperty( &mat.mSpecular, 1, AI_MATKEY_COLOR_SPECULAR);
	mat.pcInstance->AddProperty( &mat.mEmissive, 1, AI_MATKEY_COLOR_EMISSIVE);

	// shininess
	if (0.0f != mat.mSpecularExponent && 0.0f != mat.mShininessStrength)
	{
		mat.pcInstance->AddProperty( &mat.mSpecularExponent, 1, AI_MATKEY_SHININESS);
		mat.pcInstance->AddProperty( &mat.mShininessStrength, 1, AI_MATKEY_SHININESS_STRENGTH);
	}
	// if there is no shininess, we can disable phong lighting
	else if (Dot3DS::Dot3DSFile::Metal == mat.mShading ||
		Dot3DS::Dot3DSFile::Phong == mat.mShading ||
		Dot3DS::Dot3DSFile::Blinn == mat.mShading)
	{
		mat.mShading = Dot3DS::Dot3DSFile::Gouraud;
	}

	// opacity
	mat.pcInstance->AddProperty<float>( &mat.mTransparency,1,AI_MATKEY_OPACITY);


	// shading mode
	aiShadingMode eShading = aiShadingMode_NoShading;
	switch (mat.mShading)
	{
		case Dot3DS::Dot3DSFile::Flat:
			eShading = aiShadingMode_Flat; break;
		case Dot3DS::Dot3DSFile::Phong :
			eShading = aiShadingMode_Phong; break;
		case Dot3DS::Dot3DSFile::Blinn :
			eShading = aiShadingMode_Blinn; break;

		// I don't know what "Wire" shading should be,
		// assume it is simple lambertian diffuse (L dot N) shading
		case Dot3DS::Dot3DSFile::Wire:
		case Dot3DS::Dot3DSFile::Gouraud:
			eShading = aiShadingMode_Gouraud; break;
		case Dot3DS::Dot3DSFile::Metal :
			eShading = aiShadingMode_CookTorrance; break;
	}
	mat.pcInstance->AddProperty<int>( (int*)&eShading,1,AI_MATKEY_SHADING_MODEL);

	if (Dot3DS::Dot3DSFile::Wire == mat.mShading)
	{
		// set the wireframe flag
		unsigned int iWire = 1;
		mat.pcInstance->AddProperty<int>( (int*)&iWire,1,AI_MATKEY_ENABLE_WIREFRAME);
	}

	// texture, if there is one
	if( mat.sTexDiffuse.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexDiffuse.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_DIFFUSE(0));

		if (is_not_qnan(mat.sTexDiffuse.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexDiffuse.mTextureBlend, 1, 
			AI_MATKEY_TEXBLEND_DIFFUSE(0));
	}
	if( mat.sTexSpecular.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexSpecular.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_SPECULAR(0));

		if (is_not_qnan(mat.sTexSpecular.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexSpecular.mTextureBlend, 1,
			AI_MATKEY_TEXBLEND_SPECULAR(0));
	}
	if( mat.sTexOpacity.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexOpacity.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_OPACITY(0));

		if (is_not_qnan(mat.sTexOpacity.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexOpacity.mTextureBlend, 1,
			AI_MATKEY_TEXBLEND_OPACITY(0));
	}
	if( mat.sTexEmissive.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexEmissive.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_EMISSIVE(0));

		if (is_not_qnan(mat.sTexEmissive.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexEmissive.mTextureBlend, 1, 
			AI_MATKEY_TEXBLEND_EMISSIVE(0));
	}
	if( mat.sTexAmbient.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexAmbient.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_AMBIENT(0));

		if (is_not_qnan(mat.sTexAmbient.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexAmbient.mTextureBlend, 1, 
			AI_MATKEY_TEXBLEND_AMBIENT(0));
	}
	if( mat.sTexBump.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexBump.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_HEIGHT(0));

		if (is_not_qnan(mat.sTexBump.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexBump.mTextureBlend, 1,
			AI_MATKEY_TEXBLEND_HEIGHT(0));
	}
	if( mat.sTexShininess.mMapName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.sTexShininess.mMapName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_TEXTURE_SHININESS(0));

		if (is_not_qnan(mat.sTexShininess.mTextureBlend))
			mat.pcInstance->AddProperty<float>( &mat.sTexBump.mTextureBlend, 1, 
			AI_MATKEY_TEXBLEND_SHININESS(0));
	}

	// store the name of the material itself, too
	if( mat.mName.length() > 0)
	{
		aiString tex;
		tex.Set( mat.mName);
		mat.pcInstance->AddProperty( &tex, AI_MATKEY_NAME);
	}
	return;
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::ConvertMeshes(ASE::Mesh& mesh, std::vector<aiMesh*>& avOutMeshes)
{
	// validate the material index of the mesh
	if (mesh.iMaterialIndex >= this->mParser->m_vMaterials.size())
	{
		mesh.iMaterialIndex = (unsigned int)this->mParser->m_vMaterials.size()-1;
		DefaultLogger::get()->warn("Material index is out of range");
	}


	// if the material the mesh is assigned to is consisting of submeshes
	// we'll need to split it ... Quak.
	if (!this->mParser->m_vMaterials[mesh.iMaterialIndex].avSubMaterials.empty())
	{
		std::vector<ASE::Material> vSubMaterials = this->mParser->
			m_vMaterials[mesh.iMaterialIndex].avSubMaterials;

		std::vector<unsigned int>* aiSplit = new std::vector<unsigned int>[
			vSubMaterials.size()];

		// build a list of all faces per submaterial
		unsigned int iNum = 0;
		for (unsigned int i = 0; i < mesh.mFaces.size();++i)
		{
			// check range
			if (mesh.mFaces[i].iMaterial >= vSubMaterials.size())
				{
					DefaultLogger::get()->warn("Submaterial index is out of range");

					// use the last material instead
					aiSplit[vSubMaterials.size()-1].push_back(i);
				}
			else aiSplit[mesh.mFaces[i].iMaterial].push_back(i);
		}

		// now generate submeshes
		for (unsigned int p = 0; p < vSubMaterials.size();++p)
		{
			if (aiSplit[p].size() != 0)
			{
				aiMesh* p_pcOut = new aiMesh();

				// let the sub material index
				p_pcOut->mMaterialIndex = p;

				// we will need this material
				this->mParser->m_vMaterials[mesh.iMaterialIndex].avSubMaterials[p].bNeed = true;

				// store the real index here ... color channel 3
				p_pcOut->mColors[3] = (aiColor4D*)(uintptr_t)mesh.iMaterialIndex;

				// store a pointer to the mesh in color channel 2
				p_pcOut->mColors[2] = (aiColor4D*) &mesh;
				avOutMeshes.push_back(p_pcOut);

				// convert vertices
				p_pcOut->mNumVertices = (unsigned int)aiSplit[p].size()*3;
				p_pcOut->mNumFaces = (unsigned int)aiSplit[p].size();

				// receive output vertex weights
				std::vector<std::pair<unsigned int, float> >* avOutputBones;
				if (!mesh.mBones.empty())
				{
					avOutputBones = new std::vector<std::pair<unsigned int, float> >[mesh.mBones.size()];
				}
				
				// allocate enough storage for faces
				p_pcOut->mFaces = new aiFace[p_pcOut->mNumFaces];

				if (p_pcOut->mNumVertices != 0)
				{
					p_pcOut->mVertices = new aiVector3D[p_pcOut->mNumVertices];
					p_pcOut->mNormals = new aiVector3D[p_pcOut->mNumVertices];
					unsigned int iBase = 0;

					for (unsigned int q = 0; q < aiSplit[p].size();++q)
					{
						unsigned int iIndex = aiSplit[p][q];

						p_pcOut->mFaces[q].mIndices = new unsigned int[3];
						p_pcOut->mFaces[q].mNumIndices = 3;

						for (unsigned int t = 0; t < 3;++t)
						{
							const uint32_t iIndex2 = mesh.mFaces[iIndex].mIndices[t];

							p_pcOut->mVertices[iBase] = mesh.mPositions[iIndex2];
							p_pcOut->mNormals[iBase] = mesh.mNormals[iIndex2];

							// convert bones, if existing
							if (!mesh.mBones.empty())
							{
								// check whether there is a vertex weight that is using
								// this vertex index ...
								if (iIndex2 < mesh.mBoneVertices.size())
								{
									for (std::vector<std::pair<int,float> >::const_iterator
										blubb =  mesh.mBoneVertices[iIndex2].mBoneWeights.begin();
										blubb != mesh.mBoneVertices[iIndex2].mBoneWeights.end();++blubb)
									{
										// NOTE: illegal cases have already been filtered out
										avOutputBones[(*blubb).first].push_back(std::pair<unsigned int, float>(
											iBase,(*blubb).second));
									}
								}
							}
							++iBase;
						}
						p_pcOut->mFaces[q].mIndices[0] = iBase-3;
						p_pcOut->mFaces[q].mIndices[1] = iBase-2;
						p_pcOut->mFaces[q].mIndices[2] = iBase-1;
					}
				}
				// convert texture coordinates
				for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS;++c)
				{
					if (!mesh.amTexCoords[c].empty())
					{
						p_pcOut->mTextureCoords[c] = new aiVector3D[p_pcOut->mNumVertices];
						unsigned int iBase = 0;
						for (unsigned int q = 0; q < aiSplit[p].size();++q)
						{
							unsigned int iIndex = aiSplit[p][q];
							for (unsigned int t = 0; t < 3;++t)
							{
								p_pcOut->mTextureCoords[c][iBase++] = mesh.amTexCoords[c][mesh.mFaces[iIndex].mIndices[t]];
							}
						}
						// setup the number of valid vertex components
						p_pcOut->mNumUVComponents[c] = mesh.mNumUVComponents[c];
					}
				}

				// convert vertex colors (only one set supported)
				if (!mesh.mVertexColors.empty())
				{
					p_pcOut->mColors[0] = new aiColor4D[p_pcOut->mNumVertices];
					unsigned int iBase = 0;
					for (unsigned int q = 0; q < aiSplit[p].size();++q)
					{
						unsigned int iIndex = aiSplit[p][q];
						for (unsigned int t = 0; t < 3;++t)
						{
							p_pcOut->mColors[0][iBase++] = mesh.mVertexColors[mesh.mFaces[iIndex].mIndices[t]];
						}
					}
				}
				if (!mesh.mBones.empty())
				{
					p_pcOut->mNumBones = 0;
					for (unsigned int mrspock = 0; mrspock < mesh.mBones.size();++mrspock)
						if (!avOutputBones[mrspock].empty())p_pcOut->mNumBones++;

					p_pcOut->mBones = new aiBone* [ p_pcOut->mNumBones ];
					aiBone** pcBone = p_pcOut->mBones;
					for (unsigned int mrspock = 0; mrspock < mesh.mBones.size();++mrspock)
					{
						if (!avOutputBones[mrspock].empty())
						{
							// we will need this bone. add it to the output mesh and
							// add all per-vertex weights
							aiBone* pc = *pcBone = new aiBone();
							pc->mName.Set(mesh.mBones[mrspock].mName);

							pc->mNumWeights = (unsigned int)avOutputBones[mrspock].size();
							pc->mWeights = new aiVertexWeight[pc->mNumWeights];

							for (unsigned int captainkirk = 0; captainkirk < pc->mNumWeights;++captainkirk)
							{
								const std::pair<unsigned int,float>& ref = avOutputBones[mrspock][captainkirk];
								pc->mWeights[captainkirk].mVertexId = ref.first;
								pc->mWeights[captainkirk].mWeight = ref.second;
							}
							++pcBone;
						}
					}
					// delete allocated storage
					delete[] avOutputBones;
				}
			}
		}
		// delete storage
		delete[] aiSplit;
	}
	else
	{
		// otherwise we can simply copy the data to one output mesh
		aiMesh* p_pcOut = new aiMesh();

		// set an empty sub material index
		p_pcOut->mMaterialIndex = ASE::Face::DEFAULT_MATINDEX;
		this->mParser->m_vMaterials[mesh.iMaterialIndex].bNeed = true;

		// store the real index here ... in color channel 3
		p_pcOut->mColors[3] = (aiColor4D*)(uintptr_t)mesh.iMaterialIndex;

		// store a pointer to the mesh in color channel 2
		p_pcOut->mColors[2] = (aiColor4D*) &mesh;
		avOutMeshes.push_back(p_pcOut);

		// if the mesh hasn't faces or vertices, there are two cases
		// possible: 1. the model is invalid. 2. This is a dummy
		// helper object which we are going to remove later ...
		if (mesh.mFaces.empty() || mesh.mPositions.empty())
		{
			return;
		}

		// convert vertices
		p_pcOut->mNumVertices = (unsigned int)mesh.mPositions.size();
		p_pcOut->mNumFaces = (unsigned int)mesh.mFaces.size();

		// allocate enough storage for faces
		p_pcOut->mFaces = new aiFace[p_pcOut->mNumFaces];

		// copy vertices
		p_pcOut->mVertices = new aiVector3D[mesh.mPositions.size()];
		memcpy(p_pcOut->mVertices,&mesh.mPositions[0],
			mesh.mPositions.size() * sizeof(aiVector3D));

		// copy normals
		p_pcOut->mNormals = new aiVector3D[mesh.mNormals.size()];
		memcpy(p_pcOut->mNormals,&mesh.mNormals[0],
			mesh.mNormals.size() * sizeof(aiVector3D));

		// copy texture coordinates
		for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS;++c)
		{
			if (!mesh.amTexCoords[c].empty())
			{
				p_pcOut->mTextureCoords[c] = new aiVector3D[mesh.amTexCoords[c].size()];
				memcpy(p_pcOut->mTextureCoords[c],&mesh.amTexCoords[c][0],
					mesh.amTexCoords[c].size() * sizeof(aiVector3D));

				// setup the number of valid vertex components
				p_pcOut->mNumUVComponents[c] = mesh.mNumUVComponents[c];
			}
		}

		// copy vertex colors
		if (!mesh.mVertexColors.empty())
		{
			p_pcOut->mColors[0] = new aiColor4D[mesh.mVertexColors.size()];
			memcpy(p_pcOut->mColors[0],&mesh.mVertexColors[0],
				mesh.mVertexColors.size() * sizeof(aiColor4D));
		}

		// copy faces
		for (unsigned int iFace = 0; iFace < p_pcOut->mNumFaces;++iFace)
		{
			p_pcOut->mFaces[iFace].mNumIndices = 3;
			p_pcOut->mFaces[iFace].mIndices = new unsigned int[3];

			// copy indices
			p_pcOut->mFaces[iFace].mIndices[0] = mesh.mFaces[iFace].mIndices[0];
			p_pcOut->mFaces[iFace].mIndices[1] = mesh.mFaces[iFace].mIndices[1];
			p_pcOut->mFaces[iFace].mIndices[2] = mesh.mFaces[iFace].mIndices[2];
		}

		// copy vertex bones
		if (!mesh.mBones.empty() && !mesh.mBoneVertices.empty())
		{
			std::vector<aiVertexWeight>* avBonesOut = new
				std::vector<aiVertexWeight>[mesh.mBones.size()];

			// find all vertex weights for this bone
			unsigned int quak = 0;
			for (std::vector<BoneVertex>::const_iterator
				harrypotter =  mesh.mBoneVertices.begin();
				harrypotter != mesh.mBoneVertices.end();++harrypotter,++quak)
			{
				for (std::vector<std::pair<int,float> >::const_iterator
					ronaldweasley  = (*harrypotter).mBoneWeights.begin();
					ronaldweasley != (*harrypotter).mBoneWeights.end();++ronaldweasley)
				{
					aiVertexWeight weight;
					weight.mVertexId = quak;
					weight.mWeight = (*ronaldweasley).second;
					avBonesOut[(*ronaldweasley).first].push_back(weight);
				}
			}

			// now build a final bone list
			p_pcOut->mNumBones = 0;
			for (unsigned int jfkennedy = 0; jfkennedy < mesh.mBones.size();++jfkennedy)
				if (!avBonesOut[jfkennedy].empty())p_pcOut->mNumBones++;

			p_pcOut->mBones = new aiBone*[p_pcOut->mNumBones];
			aiBone** pcBone = p_pcOut->mBones;
			for (unsigned int jfkennedy = 0; jfkennedy < mesh.mBones.size();++jfkennedy)
			{
				if (!avBonesOut[jfkennedy].empty())
				{
					aiBone* pc = *pcBone = new aiBone();
					pc->mName.Set(mesh.mBones[jfkennedy].mName);
					pc->mNumWeights = (unsigned int)avBonesOut[jfkennedy].size();
					pc->mWeights = new aiVertexWeight[pc->mNumWeights];
					memcpy(pc->mWeights,&avBonesOut[jfkennedy][0],
						sizeof(aiVertexWeight) * pc->mNumWeights);
					++pcBone;
				}
			}
		}
	}
	return;
}
// ------------------------------------------------------------------------------------------------
void ComputeBounds(ASE::Mesh& mesh,aiVector3D& minVec, aiVector3D& maxVec,
				   aiMatrix4x4& matrix)
{
	minVec = aiVector3D( 1e10f, 1e10f, 1e10f);
	maxVec = aiVector3D( -1e10f, -1e10f, -1e10f);
	for( std::vector<aiVector3D>::const_iterator
		i =  mesh.mPositions.begin();
		i != mesh.mPositions.end();++i)
	{
		aiVector3D v = matrix*(*i);

		minVec.x = std::min( minVec.x, v.x);
		minVec.y = std::min( minVec.y, v.y);
		minVec.z = std::min( minVec.z, v.z);
		maxVec.x = std::max( maxVec.x, v.x);
		maxVec.y = std::max( maxVec.y, v.y);
		maxVec.z = std::max( maxVec.z, v.z);
	}
	return;
}
// ------------------------------------------------------------------------------------------------
void ASEImporter::BuildMaterialIndices()
{
	ai_assert(NULL != pcScene);

	// iterate through all materials and check whether we need them
	for (unsigned int iMat = 0; iMat < this->mParser->m_vMaterials.size();++iMat)
	{
		if (this->mParser->m_vMaterials[iMat].bNeed)
		{
			// convert it to the aiMaterial layout
			this->ConvertMaterial(this->mParser->m_vMaterials[iMat]);
			++pcScene->mNumMaterials;
		}
		for (unsigned int iSubMat = 0; iSubMat < this->mParser->m_vMaterials[
			iMat].avSubMaterials.size();++iSubMat)
		{
			if (this->mParser->m_vMaterials[iMat].avSubMaterials[iSubMat].bNeed)
			{
				// convert it to the aiMaterial layout
				this->ConvertMaterial(this->mParser->m_vMaterials[iMat].avSubMaterials[iSubMat]);
				++pcScene->mNumMaterials;
			}
		}
	}

	// allocate the output material array
	pcScene->mMaterials = new aiMaterial*[pcScene->mNumMaterials];
	Dot3DS::Material** pcIntMaterials = new Dot3DS::Material*[pcScene->mNumMaterials];

	unsigned int iNum = 0;
	for (unsigned int iMat = 0; iMat < this->mParser->m_vMaterials.size();++iMat)
	{
		if (this->mParser->m_vMaterials[iMat].bNeed)
		{
			ai_assert(NULL != this->mParser->m_vMaterials[iMat].pcInstance);
			pcScene->mMaterials[iNum] = this->mParser->m_vMaterials[iMat].pcInstance;

			// store the internal material, too
			pcIntMaterials[iNum] = &this->mParser->m_vMaterials[iMat];

			// iterate through all meshes and search for one which is using
			// this top-level material index
			for (unsigned int iMesh = 0; iMesh < pcScene->mNumMeshes;++iMesh)
			{
				if (ASE::Face::DEFAULT_MATINDEX == pcScene->mMeshes[iMesh]->mMaterialIndex &&
					iMat == (uintptr_t)pcScene->mMeshes[iMesh]->mColors[3])
				{
					pcScene->mMeshes[iMesh]->mMaterialIndex = iNum;
					pcScene->mMeshes[iMesh]->mColors[3] = NULL;
				}
			}
			iNum++;
		}
		for (unsigned int iSubMat = 0; iSubMat < this->mParser->m_vMaterials[iMat].avSubMaterials.size();++iSubMat)
		{
			if (this->mParser->m_vMaterials[iMat].avSubMaterials[iSubMat].bNeed)
			{
				ai_assert(NULL != this->mParser->m_vMaterials[iMat].avSubMaterials[iSubMat].pcInstance);
				pcScene->mMaterials[iNum] = this->mParser->m_vMaterials[iMat].
					avSubMaterials[iSubMat].pcInstance;

				// store the internal material, too
				pcIntMaterials[iNum] = &this->mParser->m_vMaterials[iMat].avSubMaterials[iSubMat];

				// iterate through all meshes and search for one which is using
				// this sub-level material index
				for (unsigned int iMesh = 0; iMesh < pcScene->mNumMeshes;++iMesh)
				{
					if (iSubMat == pcScene->mMeshes[iMesh]->mMaterialIndex &&
						iMat == (uintptr_t)pcScene->mMeshes[iMesh]->mColors[3])
					{
						pcScene->mMeshes[iMesh]->mMaterialIndex = iNum;
						pcScene->mMeshes[iMesh]->mColors[3] = NULL;
					}
				}
				iNum++;
			}
		}
	}
	// prepare for the next step
	for (unsigned int hans = 0; hans < this->mParser->m_vMaterials.size();++hans)
	{
		TextureTransform::ApplyScaleNOffset(this->mParser->m_vMaterials[hans]);
	}

	// now we need to iterate through all meshes,
	// generating correct texture coordinates and material uv indices
	for (unsigned int curie = 0; curie < pcScene->mNumMeshes;++curie)
	{
		aiMesh* pcMesh = pcScene->mMeshes[curie];

		// apply texture coordinate transformations
		TextureTransform::BakeScaleNOffset(pcMesh,pcIntMaterials[pcMesh->mMaterialIndex]);
	}
	for (unsigned int hans = 0; hans < pcScene->mNumMaterials;++hans)
	{
		// setup the correct UV indices for each material
		TextureTransform::SetupMatUVSrc(pcScene->mMaterials[hans],
			pcIntMaterials[hans]);
	}
	delete[] pcIntMaterials;

	// finished!
	return;
}
// ------------------------------------------------------------------------------------------------
// Generate normal vectors basing on smoothing groups
void ASEImporter::GenerateNormals(ASE::Mesh& mesh)
{
	if (mesh.mNormals.empty())
	{
		// need to calculate normals ... 
		// TODO: Find a way to merge this with the code in 3DSGenNormals.cpp
		mesh.mNormals.resize(mesh.mPositions.size(),aiVector3D());
		for( unsigned int a = 0; a < mesh.mFaces.size(); a++)
		{
			const ASE::Face& face = mesh.mFaces[a];

			// assume it is a triangle
			aiVector3D* pV1 = &mesh.mPositions[face.mIndices[2]];
			aiVector3D* pV2 = &mesh.mPositions[face.mIndices[1]];
			aiVector3D* pV3 = &mesh.mPositions[face.mIndices[0]];

			aiVector3D pDelta1 = *pV2 - *pV1;
			aiVector3D pDelta2 = *pV3 - *pV1;
			aiVector3D vNor = pDelta1 ^ pDelta2;

			mesh.mNormals[face.mIndices[0]] = vNor;
			mesh.mNormals[face.mIndices[1]] = vNor;
			mesh.mNormals[face.mIndices[2]] = vNor;
		}

		// calculate the position bounds so we have a reliable epsilon to 
		// check position differences against 
		// @Schrompf: This is the 7th time this snippet is repeated!
		aiVector3D minVec( 1e10f, 1e10f, 1e10f), maxVec( -1e10f, -1e10f, -1e10f);
		for( unsigned int a = 0; a < mesh.mPositions.size(); a++)
		{
			minVec.x = std::min( minVec.x, mesh.mPositions[a].x);
			minVec.y = std::min( minVec.y, mesh.mPositions[a].y);
			minVec.z = std::min( minVec.z, mesh.mPositions[a].z);
			maxVec.x = std::max( maxVec.x, mesh.mPositions[a].x);
			maxVec.y = std::max( maxVec.y, mesh.mPositions[a].y);
			maxVec.z = std::max( maxVec.z, mesh.mPositions[a].z);
		}
		const float posEpsilon = (maxVec - minVec).Length() * 1e-5f;

		std::vector<aiVector3D> avNormals;
		avNormals.resize(mesh.mNormals.size());

		// now generate the spatial sort tree
		D3DSSpatialSorter sSort;
		for( std::vector<ASE::Face>::iterator
			i =  mesh.mFaces.begin();
			i != mesh.mFaces.end();++i){sSort.AddFace(&(*i),mesh.mPositions);}
		sSort.Prepare();

		for( std::vector<ASE::Face>::iterator
			i =  mesh.mFaces.begin();
			i != mesh.mFaces.end();++i)
		{
			std::vector<unsigned int> poResult;
			for (unsigned int c = 0; c < 3;++c)
			{
				sSort.FindPositions(mesh.mPositions[(*i).mIndices[c]],(*i).iSmoothGroup,
					posEpsilon,poResult);

				aiVector3D vNormals;
				float fDiv = 0.0f;
				for (std::vector<unsigned int>::const_iterator
					a =  poResult.begin();
					a != poResult.end();++a)
				{
					vNormals += mesh.mNormals[(*a)];
					fDiv += 1.0f;
				}
				vNormals.x /= fDiv;vNormals.y /= fDiv;vNormals.z /= fDiv;
				vNormals.Normalize();
				avNormals[(*i).mIndices[c]] = vNormals;
				poResult.clear();
			}
		}
		mesh.mNormals = avNormals;
	}
	return;
}
