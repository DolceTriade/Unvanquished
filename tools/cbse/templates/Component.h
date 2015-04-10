//* Daemon CBSE Source Code
//* Copyright (c) 2014-2015, Daemon Developers
//* All rights reserved.
//*
//* Redistribution and use in source and binary forms, with or without
//* modification, are permitted provided that the following conditions are met:
//*
//* * Redistributions of source code must retain the above copyright notice, this
//*   list of conditions and the following disclaimer.
//*
//* * Redistributions in binary form must reproduce the above copyright notice,
//*   this list of conditions and the following disclaimer in the documentation
//*   and/or other materials provided with the distribution.
//*
//* * Neither the name of Daemon CBSE nor the names of its
//*   contributors may be used to endorse or promote products derived from
//*   this software without specific prior written permission.
//*
//* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
//* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
{% if header is defined %}
	/*
	{% for line in header %}
		 * {{line}}
	{% endfor %}
	 */

{% endif %}
#ifndef {{component.get_name().upper()}}_COMPONENT_H_
#define {{component.get_name().upper()}}_COMPONENT_H_

//* Make sure the backend is included, even though this might have happened before.
#include "../{{dirs['backend']}}/{{files['backend']}}"
//* Make components know each other.
#include "../{{dirs['backend']}}/{{files['components']}}"

class {{component.get_type_name()}}: public {{component.get_base_type_name()}} {
//* TODO: Uncomment after changing message handler visibility to protected.
//*	friend class Entity;

	public:
		// ///////////////////// //
		// Autogenerated Members //
		// ///////////////////// //

		/**
		 * @brief Default constructor of the {{component.get_type_name()}}.
		 * @param entity The entity that owns the component instance.
		{% for param in component.get_param_names() %}
			 * @param {{param}} An initialization parameter.
		{% endfor %}
		{% for required in component.get_required_components() %}
			 * @param r_{{required.get_type_name()}} A {{required.get_type_name()}} instance that this component depends on.
        {% endfor %}
		 * @note This method is an interface for autogenerated code, do not modify its signature.
		 */
		//* {{component.get_constructor_declaration()}};
		{{component.get_type_name()}}(Entity& entity
		{%- for declaration in component.get_param_declarations() -%}
			, {{declaration}}
		{%- endfor -%}
		{%- for declaration in component.get_required_component_declarations() -%}
			, {{declaration}}
		{%- endfor -%}
		);

//* TODO: Uncomment after changing message handler visibility to protected.
//*	protected:
		{% for message in component.get_messages_to_handle() %}
			/**
			 * @brief Handle the {{message.get_name()}} message.
			{% for param in message.get_arg_names() %}
				 * @param {{param}}
			{% endfor %}
			 * @note This method is an interface for autogenerated code, do not modify its signature.
			 */
			{{message.get_return_type()}} {{message.get_handler_declaration()}};

		{% endfor %}
		// /////////////////// //
		// Handwritten Members //
		// /////////////////// //

	private:
		// /////////////////// //
		// Handwritten Members //
		// /////////////////// //

};

#endif // {{component.get_name().upper()}}_COMPONENT_H_

//* vi:ai:ts=4
