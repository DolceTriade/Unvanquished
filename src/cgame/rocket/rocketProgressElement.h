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

#ifndef ROCKETPROGRESSELEMENT_H
#define ROCKETPROGRESSELEMENT_H

#include "rocket.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/DataSource.h>
#include "../cg_local.h"

class RocketProgressElement : public Rml::Element
{
   public:
	RocketProgressElement( const Rml::String &tag )
		: Rml::Element( tag ), value_( 0.0f ), source_( "" )
	{}

	virtual ~RocketProgressElement() {}

	void OnAttributeChange( const Rml::ElementAttributes &changed_attributes ) override
	{
		auto it = changed_attributes.find( "src" );
		if ( it != changed_attributes.end() )
		{
			source_ = it->second.Get<Rml::String>();
		}
		it = changed_attributes.find( "targetAttribute" );
		if ( it != changed_attributes.end() )
		{
			targetAttribute_ = it->second.Get<Rml::String>();
		}
		Rml::Element::OnAttributeChange( changed_attributes );
	}

	void OnUpdate() override
	{
<<<<<<< HEAD
		auto child = GetChild( 0 );
		if ( !source_.empty() && child && !targetAttribute_.empty() )
=======
		if ( !child_ )
		{
			child_ = GetChild( 0 );
		}
		if ( !source_.empty() && child_ && !targetAttribute_.empty() )
>>>>>>> b7fedce76 (rmlui: Add donut progress and make loading screen use it)
		{
			float newValue = CG_Rocket_ProgressBarValue( source_.c_str() );

			if ( newValue != value_ )
			{
<<<<<<< HEAD
				child->SetAttribute( targetAttribute_, newValue );
=======
				child_->SetAttribute( targetAttribute_, newValue );
>>>>>>> b7fedce76 (rmlui: Add donut progress and make loading screen use it)
				value_ = newValue;
			}
		}
		Rml::Element::OnUpdate();
	}

   private:
	float value_;  // current value
<<<<<<< HEAD
=======
	Rml::Element *child_;
>>>>>>> b7fedce76 (rmlui: Add donut progress and make loading screen use it)
	Rml::String source_;
	Rml::String targetAttribute_;
};

#endif
