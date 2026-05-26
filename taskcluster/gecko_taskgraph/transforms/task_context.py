from typing import Optional, Union

from taskgraph.transforms.base import TransformSequence
from taskgraph.util.schema import Schema, resolve_keyed_by
from taskgraph.util.templates import deep_get, substitute_task_fields
from taskgraph.util.yaml import load_yaml


class TaskContextConfig(Schema):
    # A list of fields in the task to substitute the provided values
    # into.
    substitution_fields: list[str]
    # Retrieve task context values from parameters. A single
    # parameter may be provided or a list of parameters in
    # priority order. The latter can be useful in implementing a
    # "default" value if some other parameter is not provided.
    from_parameters: Optional[dict[str, Union[list[str], str]]] = None
    # Retrieve task context values from a yaml file. The provided
    # file should usually only contain top level keys and values
    # (eg: nested objects will not be interpolated - they will be
    # substituted as text representations of the object).
    from_file: Optional[str] = None
    # Key/value pairs to be used as task context
    from_object: Optional[object] = None


class ResolveKeyedByConfig(Schema):
    # Dotted field names to resolve. Each is passed to `resolve_keyed_by`
    # in order. Unlisted `by-*` blocks elsewhere in the task are left
    # untouched so kinds can mix this transform with bespoke ones.
    fields: list[str]
    # Forwarded to `resolve_keyed_by`. Useful when a `by-*` key is not yet
    # available at this stage and should be resolved by a later transform.
    defer: Optional[list[str]] = None
    # Forwarded to `resolve_keyed_by`.
    enforce_single_match: Optional[bool] = None


#: Schema for the task_context transforms
class TaskContextSchema(Schema, forbid_unknown_fields=False, kw_only=True):
    name: Optional[str] = None
    # `task-context` can be used to substitute values into any field in a
    # task with data that is not known until `taskgraph` runs.
    #
    # This data can be provided via `from-parameters` or `from-file`,
    # which can pull in values from parameters and a defined yml file
    # respectively.
    #
    # Data may also be provided directly in the `from-object` section of
    # `task-context`. This can be useful in `kinds` that define most of
    # their contents in `task-defaults`, but have some values that may
    # differ for various concrete `tasks` in the `kind`.
    #
    # If the same key is found in multiple places the order of precedence
    # is as follows:
    #   - Parameters
    #   - `from-object` keys
    #   - File
    #
    # That is to say: parameters will always override anything else.
    task_context: Optional[TaskContextConfig] = None
    resolve_keyed_by: Optional[ResolveKeyedByConfig] = None


SCHEMA = TaskContextSchema

transforms = TransformSequence()
transforms.add_validate(SCHEMA)


@transforms.add
def render_task(config, tasks):
    for task in tasks:
        sub_config = task.pop("task-context", None)
        if sub_config is None:
            yield task
            continue
        params_context = {}
        for var, path in sub_config.pop("from-parameters", {}).items():
            if isinstance(path, str):
                params_context[var] = deep_get(config.params, path)
            else:
                for choice in path:
                    value = deep_get(config.params, choice)
                    if value is not None:
                        params_context[var] = value
                        break

        file_context = {}
        from_file = sub_config.pop("from-file", None)
        if from_file:
            file_context = load_yaml(from_file)

        fields = sub_config.pop("substitution-fields")

        subs = {}
        subs.update(file_context)
        # We've popped away the configuration; everything left in `sub_config` is
        # substitution key/value pairs.
        subs.update(sub_config.pop("from-object", {}))
        subs.update(params_context)
        if "name" in task:
            subs.setdefault("name", task["name"])

        # Now that we have our combined context, we can substitute.
        substitute_task_fields(task, fields, **subs)
        yield task


@transforms.add
def resolve(config, tasks):
    for task in tasks:
        sub_config = task.pop("resolve-keyed-by", None)
        if sub_config is None:
            yield task
            continue

        item_name = task.get("name") or task.get("label", "?")
        subs = {}
        for k, v in config.params.items():
            # don't include dicts, lists, etc. as possible su
            if isinstance(v, (str, int)):
                subs[k.replace("_", "-")] = v

        kwargs = {}
        if sub_config.get("defer") is not None:
            kwargs["defer"] = sub_config["defer"]
        if sub_config.get("enforce-single-match") is not None:
            kwargs["enforce_single_match"] = sub_config["enforce-single-match"]

        for field in sub_config["fields"]:
            resolve_keyed_by(task, field, item_name, **kwargs, **subs)

        yield task
