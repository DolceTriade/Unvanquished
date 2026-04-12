/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2026 Unvanquished Developers

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Unvanquished Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Unvanquished Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following the
terms and conditions of the GNU General Public License which accompanied the Unvanquished
Source Code.  If not, please request a copy in writing from id Software at the address
below.

If you have questions concerning this license or the applicable additional terms, you
may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville,
Maryland 20850 USA.

===========================================================================
*/

#ifndef ROCKETDONUTPROGRESS_H
#define ROCKETDONUTPROGRESS_H

#include "rocket.h"
#include <RmlUi/Core.h>
#include "../cg_local.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class RocketDonutProgress : public Rml::Element
{
   public:
	RocketDonutProgress( const Rml::String &tag )
		: Rml::Element( tag ),
		  ri_( Rml::GetRenderInterface() ),
		  dirty_( true ),
		  innerRadius_( 0.0f ),
		  outerRadius_( 0.0f ),
		  progress_( 0.0f ),
		  colour_( 255, 255, 255, 255 ),
		  handle_( 0 )
	{}

	virtual ~RocketDonutProgress()
	{
		if ( handle_ )
		{
			ri_->ReleaseCompiledGeometry( handle_ );
		}
	}

	virtual void OnPropertyChange( const Rml::PropertyIdSet &changed_properties ) override
	{
		Element::OnPropertyChange( changed_properties );
		if ( changed_properties.Contains( UnvPropertyId::InnerRadius ) )
		{
			innerRadius_ = std::max( 0.0f, ResolveNumericProperty( "innerRadius" ) );
			dirty_ = true;
		}
		if ( changed_properties.Contains( UnvPropertyId::OuterRadius ) )
		{
			outerRadius_ = std::max( innerRadius_, ResolveNumericProperty( "outerRadius" ) );
			dirty_ = true;
		}
        if ( changed_properties.Contains( Rml::PropertyId::Color ) )
        {
            colour_ = GetProperty( Rml::PropertyId::Color )->Get<Rml::Colourb>();
            dirty_ = true;
        }
	}

	virtual void OnAttributeChange( const Rml::ElementAttributes &changed_attributes ) override
	{
		Element::OnAttributeChange( changed_attributes );
		auto it = changed_attributes.find( "value" );
		if ( it != changed_attributes.end() )
		{
			progress_ = Math::Clamp( GetAttribute( "value" )->Get<float>(), 0.0f, 1.0f );
			dirty_ = true;
		}
	}

    bool GetIntrinsicDimensions( Rml::Vector2f &dimension, float& /*ratio*/ ) override
    {
        dimension.x = outerRadius_ * 2.0f;
        dimension.y = outerRadius_ * 2.0f;
        return true;
    }

	virtual void OnUpdate() override
	{
		constexpr int SEGMENTS = 64;
		if ( !dirty_ )
		{
			return;
		}

		if ( handle_ )
		{
			ri_->ReleaseCompiledGeometry( handle_ );
		}

		dirty_ = false;
		if ( progress_ <= 0.0f || outerRadius_ <= 0.0f || innerRadius_ >= outerRadius_ )
		{
			handle_ = 0;
			return;
		}

		const float angleStep = ( 2.0f * static_cast<float>( M_PI ) ) / static_cast<float>( SEGMENTS );
		const float startAngle = -0.5f * static_cast<float>( M_PI );
		const float maxAngle = progress_ * 2.0f * static_cast<float>( M_PI );
		const bool fullRing = progress_ >= 1.0f;
		const int ringPoints = fullRing ? SEGMENTS : std::max( 2, static_cast<int>( std::ceil( maxAngle / angleStep ) ) + 1 );
		const int quadCount = fullRing ? ringPoints : ringPoints - 1;
		std::vector<Rml::Vertex> verts;
		std::vector<int> indices;
		verts.reserve( ringPoints * 2 );
		indices.reserve( quadCount * 6 );

		for ( int i = 0; i < ringPoints; ++i )
		{
			const float angle = fullRing ? startAngle + ( i * angleStep ) : startAngle + std::min( i * angleStep, maxAngle );
			const float sin = std::sin( angle );
			const float cos = std::cos( angle );

			Rml::Vertex inner;
			inner.position.x = cos * innerRadius_;
			inner.position.y = sin * innerRadius_;
			inner.tex_coord.x = 0.0f;
			inner.tex_coord.y = static_cast<float>( i ) / std::max( 1, ringPoints - 1 );
			inner.colour = colour_;
			verts.push_back( inner );

			Rml::Vertex outer;
			outer.position.x = cos * outerRadius_;
			outer.position.y = sin * outerRadius_;
			outer.tex_coord.x = 1.0f;
			outer.tex_coord.y = static_cast<float>( i ) / std::max( 1, ringPoints - 1 );
			outer.colour = colour_;
			verts.push_back( outer );
		}

		for ( int i = 0; i < quadCount; ++i )
		{
			const int next = fullRing ? ( i + 1 ) % ringPoints : i + 1;
			uint32_t i0 = i * 2;
			uint32_t i1 = i * 2 + 1;
			uint32_t i2 = next * 2;
			uint32_t i3 = next * 2 + 1;
			indices.push_back( i0 );
			indices.push_back( i1 );
			indices.push_back( i2 );
			indices.push_back( i1 );
			indices.push_back( i3 );
			indices.push_back( i2 );
		}

		handle_ = ri_->CompileGeometry( verts.data(), static_cast<int>( verts.size() ),
		                                indices.data(), static_cast<int>( indices.size() ), 0 );
	}

	virtual void OnRender() override
	{
		if ( !handle_ )
		{
			return;
		}
		const Rml::Vector2f offset = GetAbsoluteOffset(Rml::Box::Area::CONTENT);
		const Rml::Vector2f size = GetBox().GetSize();
		const Rml::Vector2f translation( offset.x + size.x * 0.5f, offset.y + size.y * 0.5f );

		ri_->RenderCompiledGeometry( handle_, translation );
	}

   private:
	Rml::RenderInterface *ri_;
	bool dirty_;
	float innerRadius_;
	float outerRadius_;
	float progress_;
    Rml::Colourb colour_;
	Rml::CompiledGeometryHandle handle_;
};

#endif  // ROCKETDONUTPROGRESS_H
